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

  var status: ProcessingState = .inactive

  var applyConfigTask: Task<Void, Never>?

  // MARK: - Init

  init(
    engine: DSPEngine, devices: AudioDeviceManager, settings: AudioSettings,
    pipeline: PipelineStore, monitoring: MonitoringController
  ) {
    self.engine = engine
    self.devices = devices
    self.settings = settings
    self.pipeline = pipeline

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
    let captureConfig = CaptureDeviceConfig(
      type: .coreAudio,
      channels: 2,
      device: devices.captureConfig.deviceName
    )

    let playbackConfig = PlaybackDeviceConfig(
      type: .coreAudio,
      channels: 2,
      device: devices.playbackConfig.deviceName,
      exclusive: devices.exclusiveMode
    )

    var devicesConfig = DevicesConfig(
      samplerate: devices.playbackConfig.sampleRate,
      chunksize: settings.chunkSize,
      capture: captureConfig,
      playback: playbackConfig
    )

    if settings.silenceTimeout > 0 {
      devicesConfig.silenceThreshold = Double(settings.silenceThreshold)
      devicesConfig.silenceTimeout = Double(settings.silenceTimeout)
    }

    if settings.resamplerEnabled {
      devicesConfig.captureSamplerate = devices.captureConfig.sampleRate
      let resampler = ResamplerConfig(type: .synchronous)
      devicesConfig.resampler = resampler
    }

    if settings.enableRateAdjust { devicesConfig.enableRateAdjust = true }

    var filters: [String: FilterConfig] = [:]
    var mixers: [String: MixerConfig] = [:]
    var pipelineSteps: [PipelineStep] = []

    for stage in pipeline.stages {
      let stageFilters = stage.buildFilters()
      let stageMixers = stage.buildMixers()
      let stageSteps = stage.buildPipelineSteps()
      for (k, v) in stageFilters { filters[k] = v }
      for (k, v) in stageMixers { mixers[k] = v }
      pipelineSteps.append(contentsOf: stageSteps)
      if stage.type == .eq && stage.isActive {
        let eqFilters = stage.buildEQFilters(presets: pipeline.eqPresets)
        let eqSteps = stage.buildEQPipelineSteps(presets: pipeline.eqPresets)
        for (k, v) in eqFilters { filters[k] = v }
        pipelineSteps.append(contentsOf: eqSteps)
      }

    }

    var config = DSPConfiguration(devices: devicesConfig)
    if !filters.isEmpty { config.filters = filters }
    if !mixers.isEmpty { config.mixers = mixers }
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
