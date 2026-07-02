// DSPEngineController - DSP engine lifecycle and config building

import DSPConfig
import DSPLib
import Observation
import SwiftUI

@MainActor
@Observable
final class DSPEngineController {
  let engine: DSPEngine
  let devices: AudioDeviceManager
  let settings: AudioSettings
  let pipeline: PipelineStore
  let levels: LevelState

  var status: ProcessingState = .inactive

  var applyConfigTask: Task<Void, Never>?

  // MARK: - Init

  init(
    engine: DSPEngine, devices: AudioDeviceManager, settings: AudioSettings,
    pipeline: PipelineStore, monitoring: MonitoringController,
    levels: LevelState
  ) {
    self.engine = engine
    self.devices = devices
    self.settings = settings
    self.pipeline = pipeline
    self.levels = levels

    // Wire monitoring → controller callbacks, breaking the circular reference.
    monitoring.onStatusChange = { [weak self] newStatus in
      guard let self, newStatus != self.status else { return }
      self.status = newStatus
    }
    monitoring.onRestartEngine = { [weak self] in
      self?.startEngine()
    }
  }

  // MARK: - Engine Lifecycle

  func startEngine() {
    runApplyConfigTask()
  }

  func stopEngine() {
    applyConfigTask?.cancel()
    let applyTask = applyConfigTask
    applyConfigTask = nil

    levels.reset(
      captureChannels: devices.captureConfig.channels,
      playbackChannels: devices.playbackConfig.channels
    )
    Task {
      await applyTask?.value
      await engine.stop()
    }
  }

  // MARK: - Configuration Management

  func applyConfig() {
    guard status != .inactive else { return }
    runApplyConfigTask()
  }

  // MARK: - Volume / Mute

  func setVolume(_ db: Float) {
    settings.volume = db
    Task { await engine.setVolume(db) }
  }

  func toggleMute() {
    settings.isMuted.toggle()
    Task { await engine.setMute(settings.isMuted) }
  }

  // MARK: - Config Building

  func buildConfig() -> DSPConfiguration {
    var captureConfig = CaptureDeviceConfig(
      type: .coreAudio,
      channels: devices.captureConfig.channels,
      device: devices.captureConfig.deviceName
    )
    if DSPEngine.isSwiftEngine {
      captureConfig.bypassDoP = devices.captureConfig.bypassDoP
      captureConfig.dopCutoffHz = devices.captureConfig.dopCutoffHz
    }

    var playbackConfig = PlaybackDeviceConfig(
      type: .coreAudio,
      channels: devices.playbackConfig.channels,
      device: devices.playbackConfig.deviceName,
      exclusive: devices.exclusiveMode
    )
    if DSPEngine.isSwiftEngine {
      playbackConfig.outputDoP = devices.playbackConfig.outputDoP
      playbackConfig.dopEncoderFilter = devices.playbackConfig.dopEncoderFilter
    }

    var devicesConfig = DevicesConfig(
      samplerate: devices.playbackConfig.sampleRate,
      chunksize: settings.chunkSize,
      capture: captureConfig,
      playback: playbackConfig
    )

    devicesConfig.queuelimit = settings.queuelimit
    devicesConfig.stopOnRateChange = settings.stopOnRateChange
    devicesConfig.rateMeasureInterval = settings.rateMeasureInterval
    devicesConfig.multithreaded = settings.multithreaded
    if settings.multithreaded, settings.workerThreads > 0 {
      devicesConfig.workerThreads = settings.workerThreads
    }

    if settings.silenceTimeout > 0 {
      devicesConfig.silenceThreshold = Double(settings.silenceThreshold)
      devicesConfig.silenceTimeout = Double(settings.silenceTimeout)
    }

    if settings.resamplerEnabled {
      devicesConfig.captureSamplerate = devices.captureConfig.sampleRate
      // Per-engine fallbacks for resampler types the running engine
      // doesn't implement:
      //   * Swift engine → only `.synchronous` and `.apple` are
      //     implemented natively. `.asyncSinc` / `.asyncPoly` map onto
      //     `.synchronous`.
      //   * Rust engine → only the rubato-native types and
      //     `.synchronous` are implemented. `.apple` (the Core Audio
      //     wrapper) maps onto `.asyncSinc`.
      let effectiveType: ResamplerType
      if DSPEngine.isSwiftEngine {
        switch settings.resamplerType {
        case .asyncSinc, .asyncPoly: effectiveType = .synchronous
        case .synchronous, .apple: effectiveType = settings.resamplerType
        }
      } else {
        effectiveType = settings.resamplerType == .apple ? .asyncSinc : settings.resamplerType
      }
      let configResamplerType =
        DSPConfig.ResamplerType(rawValue: effectiveType.rawValue) ?? .synchronous
      var resampler = ResamplerConfig(type: configResamplerType)
      switch effectiveType {
      case .asyncSinc:
        if settings.resamplerUseProfile {
          resampler.profile = settings.resamplerProfile.rawValue
        } else {
          resampler.sincLen = settings.resamplerSincLen
          resampler.oversamplingFactor = settings.resamplerOversamplingFactor
          resampler.window = settings.resamplerWindow
          resampler.fCutoff = settings.resamplerFCutoff
        }
      case .asyncPoly:
        resampler.interpolation = settings.resamplerInterpolation.rawValue
      case .synchronous:
        break
      case .apple:
        resampler.appleQuality =
          AppleResamplerQuality(rawValue: settings.resamplerAppleQuality.rawValue) ?? .high
        resampler.appleComplexity =
          AppleResamplerComplexity(rawValue: settings.resamplerAppleComplexity.rawValue) ?? .normal
      }
      devicesConfig.resampler = resampler
    }

    if settings.enableRateAdjust { devicesConfig.enableRateAdjust = true }

    var filters: [String: FilterConfig] = [:]
    var mixers: [String: MixerConfig] = [:]
    var processors: [String: ProcessorConfig] = [:]
    var pipelineSteps: [PipelineStep] = []
    var currentChannels = devices.captureConfig.channels

    let rate = devices.captureConfig.sampleRate

    for stage in pipeline.stages {
      let stageFilters = stage.buildFilters(
        eqPresets: pipeline.eqPresets,
        convPresets: pipeline.convPresets,
        sampleRate: rate
      )
      let stageMixers = stage.buildMixers(channels: currentChannels)
      let stageProcessors = stage.buildProcessors(channels: currentChannels)
      let stageSteps = stage.buildPipelineSteps(
        eqPresets: pipeline.eqPresets,
        convPresets: pipeline.convPresets,
        sampleRate: rate
      )
      for (k, v) in stageFilters { filters[k] = v }
      for (k, v) in stageMixers { mixers[k] = v }
      for (k, v) in stageProcessors { processors[k] = v }
      pipelineSteps.append(contentsOf: stageSteps)

      // Track channel count changes through active mixers
      if stage.isActive && stage.type == .mixer {
        currentChannels = stage.mixerChannelsOut
      }
    }

    var config = DSPConfiguration(devices: devicesConfig)
    if !filters.isEmpty { config.filters = filters }
    if !mixers.isEmpty { config.mixers = mixers }
    if !processors.isEmpty { config.processors = processors }
    if !pipelineSteps.isEmpty { config.pipeline = pipelineSteps }

    return config
  }

  // MARK: - Private

  private func runApplyConfigTask() {
    guard devices.devicesAvailable() else { return }
    applyConfigTask?.cancel()
    applyConfigTask = Task {
      try? await Task.sleep(nanoseconds: 10_000_000)
      guard !Task.isCancelled else { return }
      await applyConfigAsync()
    }
  }

  private func apply(config: DSPConfiguration) async throws {
    let encoder = JSONEncoder()
    let data = try encoder.encode(config)
    guard let json = String(data: data, encoding: .utf8) else {
      throw AudioBackendError.configParse(message: "Failed to convert JSON data to UTF-8 string")
    }
    try await engine.start(configJson: json)
  }

  private func applyConfigAsync() async {
    pipeline.savePipelineStages()

    do {
      // Prime faders BEFORE sending config so the pipeline initialises at the right
      // level and doesn't see a difference that triggers a 0 dBFS ramp.
      await engine.setMute(settings.isMuted)
      await engine.setVolume(settings.volume)

      if devices.captureConfig.channels < 2 || devices.playbackConfig.channels < 2 {
        throw AudioBackendError.configParse(
          message:
            "Capture and Playback devices must have at least 2 channels selected for 2in-2out flow (Capture: \(devices.captureConfig.channels), Playback: \(devices.playbackConfig.channels))."
        )
      }

      let config = buildConfig()
      try await apply(config: config)
    } catch {
      print("[DSPEngineController] Config apply failed: \(error.localizedDescription)")
    }
  }
}
