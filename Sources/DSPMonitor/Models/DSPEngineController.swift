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

  func setFaderVolume(fader: Fader, db: Float) {
    settings.setVolume(db, for: fader)
    Task { await engine.setFaderVolume(fader, db) }
  }

  func toggleFaderMute(fader: Fader) {
    let newMute = !settings.isMuted(for: fader)
    settings.setMuted(newMute, for: fader)
    Task { await engine.setFaderMute(fader, newMute) }
  }

  // MARK: - Config Building

  func buildConfig() -> DSPConfiguration {
    var captureConfig: CaptureDeviceConfig
    switch devices.captureConfig.backend {
    case .coreAudio:
      var coreAudioCfg = CoreAudioCaptureConfig(
        channels: devices.captureConfig.channels,
        device: devices.captureConfig.deviceName
      )
      if !DSPEngine.isRustEngine {
        coreAudioCfg.bypassDoP = devices.captureConfig.bypassDoP
        coreAudioCfg.dopCutoffHz = devices.captureConfig.dopCutoffHz
      }
      captureConfig = .coreAudio(coreAudioCfg)
    case .rawFile:
      captureConfig = .rawFile(
        RawFileCaptureConfig(
          channels: devices.captureConfig.channels,
          filename: devices.captureConfig.filename.isEmpty ? "" : devices.captureConfig.filename,
          format: devices.captureConfig.fileFormat,
          skipBytes: devices.captureConfig.skipBytes > 0 ? devices.captureConfig.skipBytes : nil,
          readBytes: devices.captureConfig.readBytes > 0 ? devices.captureConfig.readBytes : nil,
          extraSamples: devices.captureConfig.extraSamples > 0
            ? devices.captureConfig.extraSamples : nil
        ))
    case .wavFile:
      captureConfig = .wavFile(
        WavFileCaptureConfig(
          filename: devices.captureConfig.filename.isEmpty ? "" : devices.captureConfig.filename,
          extraSamples: devices.captureConfig.extraSamples > 0
            ? devices.captureConfig.extraSamples : nil
        ))
    case .signalGenerator:
      captureConfig = .signalGenerator(
        GeneratorCaptureConfig(
          channels: devices.captureConfig.channels,
          signal: GeneratorConfig(
            type: devices.captureConfig.generatorType,
            freq: devices.captureConfig.generatorType == "WhiteNoise"
              ? nil : devices.captureConfig.generatorFreq,
            level: devices.captureConfig.generatorLevel
          )
        ))
    }

    var playbackConfig: PlaybackDeviceConfig
    switch devices.playbackConfig.backend {
    case .coreAudio:
      var coreAudioCfg = CoreAudioPlaybackConfig(
        channels: devices.playbackConfig.channels,
        device: devices.playbackConfig.deviceName,
        exclusive: devices.exclusiveMode
      )
      if !DSPEngine.isRustEngine {
        coreAudioCfg.outputDoP = devices.playbackConfig.outputDoP
        coreAudioCfg.dsdEncoderFilter = devices.playbackConfig.dsdEncoderFilter
      }
      playbackConfig = .coreAudio(coreAudioCfg)
    case .rawFile:
      playbackConfig = .rawFile(
        RawFilePlaybackConfig(
          channels: devices.playbackConfig.channels,
          filename: devices.playbackConfig.filename.isEmpty ? "" : devices.playbackConfig.filename,
          format: devices.playbackConfig.fileFormat,
          wavHeader: false
        ))
    case .wavFile:
      playbackConfig = .rawFile(
        RawFilePlaybackConfig(
          channels: devices.playbackConfig.channels,
          filename: devices.playbackConfig.filename.isEmpty ? "" : devices.playbackConfig.filename,
          format: devices.playbackConfig.fileFormat,
          wavHeader: true
        ))
    case .signalGenerator:
      playbackConfig = .coreAudio(
        CoreAudioPlaybackConfig(
          channels: devices.playbackConfig.channels,
          device: devices.playbackConfig.deviceName,
          exclusive: devices.exclusiveMode
        ))
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
      //   * Swift engine → supports all resampler types natively.
      //   * Rust engine → only the rubato-native types and
      //     `.synchronous` are implemented. `.apple` (the Core Audio
      //     wrapper) maps onto `.asyncSinc`.
      let effectiveType: ResamplerType
      if DSPEngine.isRustEngine {
        effectiveType = settings.resamplerType == .apple ? .asyncSinc : settings.resamplerType
      } else {
        effectiveType = settings.resamplerType
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
          resampler.interpolation = settings.resamplerSincInterpolation.rawValue
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
        channelCount: currentChannels,
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
    encoder.outputFormatting = .withoutEscapingSlashes
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
      for fader in Fader.allCases {
        await engine.setFaderMute(fader, settings.isMuted(for: fader))
        await engine.setFaderVolume(fader, settings.volume(for: fader))
      }

      let config = buildConfig()
      try await apply(config: config)
    } catch {
      print("[DSPEngineController] Config apply failed: \(error.localizedDescription)")
    }
  }
}
