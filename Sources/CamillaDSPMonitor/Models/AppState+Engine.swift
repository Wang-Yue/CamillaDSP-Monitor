// AppState+Engine - DSP engine control and config building

import CamillaDSPLib
import SwiftUI

extension AppState {

  func startEngine() {
    guard !isRunning else { return }
    lastError = nil
    lastAppliedConfigYAML = nil
    guard devicesAvailable() else { return }

    // Start the audio tap BEFORE CamillaDSP claims the capture device,
    // so AVAudioEngine gets shared access to the input.
    recreateSpectrumAnalyzer()
    startAudioCapture()

    Task {
      do {
        try await engine.connect(binaryPath: camillaDSPPath)
        let config = buildConfigDict()
        try await startEngineWithConfig(config)
        isRunning = true
        engineState = .running
        await engine.setMute(isMuted)
        await engine.setVolume(volume)
        startMonitoringTimer()
        // Retry the tap in case the early start didn't get audio
        scheduleSpectrumRestart()
      } catch {
        lastError = "\(error)"
        engineState = .inactive
        stopAudioCapture()
        await engine.disconnect()
      }
    }
  }

  func stopEngine() {
    isRunning = false
    engineState = .inactive
    stopMonitoring()
    meters.reset()
    lastAppliedConfigYAML = nil
    Task { await engine.stop() }
  }

  func applyConfig() {
    guard !isLoadingPreferences else { return }
    Task { await applyConfigAsync() }
  }

  func applyConfigAsync() async {
    guard isRunning && !isApplyingConfig && !isLoadingPreferences else { return }
    savePipelineStages()

    let config = buildConfigDict()
    let data = try? JSONSerialization.data(withJSONObject: config)
    let json = data.flatMap { String(data: $0, encoding: .utf8) } ?? ""
    if json == lastAppliedConfigYAML { return }

    isApplyingConfig = true
    do {
      try await startEngineWithConfig(config)
      lastAppliedConfigYAML = json
    } catch {
      print("[AppState] Config apply failed: \(error)")
      lastAppliedConfigYAML = nil  // allow retry on next attempt
    }
    // Wait for engine to stabilize, then verify it's running
    try? await Task.sleep(nanoseconds: 300_000_000)
    await verifyEngineRunning(config: config)
    isApplyingConfig = false

    // CamillaDSP pipeline restart can disrupt the CoreAudioTap
    scheduleSpectrumRestart()
  }

  private func verifyEngineRunning(config: [String: Any]) async {
    let state: String? = try? await engine.sendCommand("GetState")
    if state == "Running" { return }

    print("[AppState] Engine state after config: \(state ?? "Unknown"), retrying...")
    let reason: String? = try? await engine.sendCommand("GetStopReason")
    if let stopReason = reason { print("[AppState] Stop reason: \(stopReason)") }

    // Retry once
    _ = try? await startEngineWithConfig(config)
    try? await Task.sleep(nanoseconds: 300_000_000)
  }

  func setVolume(_ db: Double) {
    volume = db
    Task { await engine.setVolume(db) }
  }
  func toggleMute() {
    isMuted.toggle()
    Task { await engine.setMute(isMuted) }
  }

  private func startEngineWithConfig(_ config: [String: Any]) async throws {
    let data = try JSONSerialization.data(withJSONObject: config)
    guard let json = String(data: data, encoding: .utf8) else {
      throw AudioBackendError.commandFailed("Failed to serialize config to JSON string")
    }
    try await engine.start(configJson: json)
  }

  func buildConfigDict() -> [String: Any] {
    var devices: [String: Any] = [
      "samplerate": playbackSampleRate, "chunksize": chunkSize,
      "capture": [
        "type": "CoreAudio", "channels": captureChannels, "device": selectedCaptureDevice as Any,
        "format": captureFormat,
      ],
      "playback": [
        "type": "CoreAudio", "channels": playbackChannels, "device": selectedPlaybackDevice as Any,
        "format": playbackFormat, "exclusive": exclusiveMode,
      ],
    ]

    if resamplerEnabled {
      devices["capture_samplerate"] = captureSampleRate
      switch resamplerType {
      case .asyncSinc:
        devices["resampler"] = ["type": "AsyncSinc", "profile": resamplerProfile.rawValue]
      case .asyncPoly:
        devices["resampler"] = ["type": "AsyncPoly", "interpolation": "Cubic"]
      case .synchronous:
        devices["resampler"] = ["type": "Synchronous"]
      }
    }

    if enableRateAdjust { devices["enable_rate_adjust"] = true }

    var filters: [String: Any] = [:]
    var mixers: [String: Any] = [:]
    var pipeline: [[String: Any]] = []

    for stage in stages {
      let stageFilters = stage.buildFilters()
      let stageMixers = stage.buildMixers()
      let stageSteps = stage.buildPipelineSteps()
      for (k, v) in stageFilters { filters[k] = v }
      for (k, v) in stageMixers { mixers[k] = v }
      pipeline.append(contentsOf: stageSteps)
      if stage.type == .eq && stage.isActive {
        let eqFilters = stage.buildEQFilters(presets: eqPresets)
        let eqSteps = stage.buildEQPipelineSteps(presets: eqPresets)
        for (k, v) in eqFilters { filters[k] = v }
        pipeline.append(contentsOf: eqSteps)
      }
    }

    var config: [String: Any] = ["devices": devices]
    if !filters.isEmpty { config["filters"] = filters }
    if !mixers.isEmpty { config["mixers"] = mixers }
    if !pipeline.isEmpty { config["pipeline"] = pipeline }
    return config
  }
}
