// DSPEngineController - DSP engine lifecycle and config building

import CamillaDSPLib
import Observation
import SwiftUI

@MainActor
@Observable
final class DSPEngineController {
  let engine: DSPEngine
  let devices: AudioDeviceManager
  let settings: AudioSettings
  let pipeline: PipelineStore
  let monitoring: MonitoringController
  let levels: LevelState

  var status: AppStatus = .inactive

  var startEngineTask: Task<Void, Never>?
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
    self.monitoring = monitoring
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
    if status == .running { return }
    guard devices.devicesAvailable() else { return }

    startEngineTask?.cancel()
    startEngineTask = Task {
      do {
        try await engine.connect(binaryPath: settings.camillaDSPPath)
        guard !Task.isCancelled else { return }

        // Prime faders BEFORE sending config so the pipeline initialises at the right
        // level and doesn't see a difference that triggers a 0 dBFS ramp.
        await engine.setFaderMute(fader: 0, mute: settings.isMuted)
        await engine.setFaderExternalVolume(fader: 0, db: settings.volume)
        guard !Task.isCancelled else { return }

        let config = buildConfigDict()
        try await startEngineWithConfig(config)
      } catch {
        guard !Task.isCancelled else { return }
        await engine.disconnect()
      }
    }
  }

  func stopEngine() {
    startEngineTask?.cancel()
    applyConfigTask?.cancel()
    let startTask = startEngineTask
    let applyTask = applyConfigTask
    startEngineTask = nil
    applyConfigTask = nil

    levels.reset()
    Task {
      await startTask?.value
      await applyTask?.value
      await engine.stop()
    }
  }

  // MARK: - Configuration Management

  func applyConfig() {
    if status != .running { return }
    applyConfigTask?.cancel()
    applyConfigTask = Task {
      try? await Task.sleep(nanoseconds: 50_000_000)
      guard !Task.isCancelled else { return }
      await applyConfigAsync()
    }
  }

  func applyConfigAsync() async {
    pipeline.savePipelineStages()

    do {
      await engine.setFaderMute(fader: 0, mute: settings.isMuted)
      await engine.setFaderExternalVolume(fader: 0, db: settings.volume)

      let config = buildConfigDict()
      try await startEngineWithConfig(config)
    } catch {
      print("[DSPEngineController] Config apply failed: \(error)")
    }
  }

  // MARK: - Volume / Mute

  func setVolume(_ db: Double) {
    settings.volume = db
    Task { await engine.setVolume(db) }
  }

  func toggleMute() {
    settings.isMuted.toggle()
    Task { await engine.setMute(settings.isMuted) }
  }

  // MARK: - Config Building

  func buildConfigDict() -> [String: Any] {
    var devicesDict: [String: Any] = [
      "samplerate": devices.playbackConfig.sampleRate,
      "chunksize": settings.chunkSize,
      "volume_ramp_time": 200.0,
      "capture": [
        "type": "CoreAudio", "channels": devices.captureConfig.channels,
        "device": devices.captureConfig.deviceName as Any,
        "format": devices.captureConfig.format,
      ],
      "playback": [
        "type": "CoreAudio", "channels": devices.playbackConfig.channels,
        "device": devices.playbackConfig.deviceName as Any,
        "format": devices.playbackConfig.format, "exclusive": devices.exclusiveMode,
      ],
    ]

    if settings.resamplerEnabled {
      devicesDict["capture_samplerate"] = devices.captureConfig.sampleRate
      switch settings.resamplerType {
      case .asyncSinc:
        devicesDict["resampler"] = [
          "type": "AsyncSinc", "profile": settings.resamplerProfile.rawValue,
        ]
      case .asyncPoly:
        devicesDict["resampler"] = [
          "type": "AsyncPoly", "interpolation": settings.resamplerInterpolation.rawValue,
        ]
      case .synchronous:
        devicesDict["resampler"] = ["type": "Synchronous"]
      }
    }

    if settings.enableRateAdjust { devicesDict["enable_rate_adjust"] = true }

    var filters: [String: Any] = [:]
    var mixers: [String: Any] = [:]
    var pipelineSteps: [[String: Any]] = []

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

    var config: [String: Any] = ["devices": devicesDict]
    if !filters.isEmpty { config["filters"] = filters }
    if !mixers.isEmpty { config["mixers"] = mixers }
    if !pipelineSteps.isEmpty { config["pipeline"] = pipelineSteps }

    return config
  }

  // MARK: - Private

  private func startEngineWithConfig(_ config: [String: Any]) async throws {
    let data = try JSONSerialization.data(withJSONObject: config)
    guard let json = String(data: data, encoding: .utf8) else {
      throw AudioBackendError.commandFailed("Failed to serialize config to JSON string")
    }
    try await engine.start(configJson: json)
  }
}
