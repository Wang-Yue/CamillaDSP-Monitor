// AppState+Engine - DSP engine control and config building

import CamillaDSPLib
import SwiftUI

extension AppState {

  // MARK: - Engine Lifecycle

  func startEngine() {
    switch status {
    case .inactive, .error:
      break
    default:
      return
    }

    status = .starting
    lastError = nil
    lastAppliedConfigYAML = nil

    guard devicesAvailable() else {
      status = .error("Audio devices not available")
      return
    }

    recreateSpectrumAnalyzer()
    startAudioCapture()

    startEngineTask?.cancel()
    startEngineTask = Task {
      do {
        // 1. Connect
        try await engine.connect(binaryPath: camillaDSPPath)
        guard !Task.isCancelled else { return }

        // 2. Priming Volume/Mute BEFORE sending config.
        // CRITICAL: We use setFaderExternalVolume because it updates BOTH target_volume
        // and current_volume in CamillaDSP. This ensures that when the pipeline
        // initializes, it doesn't see a difference that triggers a 0dBFS ramp.
        await engine.setFaderMute(fader: 0, mute: isMuted)
        await engine.setFaderExternalVolume(fader: 0, db: volume)
        guard !Task.isCancelled else { return }

        // 3. Apply the configuration
        lastAppliedConfigYAML = nil
        let config = buildConfigDict()
        try await startEngineWithConfig(config)
        guard !Task.isCancelled else { return }

        status = .running
        startMonitoringTimer()
        scheduleSpectrumRestart()
      } catch {
        guard !Task.isCancelled else { return }
        lastError = "\(error)"
        status = .error("\(error)")
        stopMonitoring()
        await engine.disconnect()
      }
    }
  }

  func stopEngine() {
    // Cancel any in-progress start/apply tasks and wait for them to acknowledge cancellation
    startEngineTask?.cancel()
    applyConfigTask?.cancel()
    let startTask = startEngineTask
    let applyTask = applyConfigTask
    startEngineTask = nil
    applyConfigTask = nil
    status = .inactive
    stopMonitoring()
    levels.reset()
    spectrum.reset()
    load.reset()
    lastAppliedConfigYAML = nil
    lastRecoveryTime = nil
    Task {
      // Await cancelled tasks to ensure they've stopped before we stop the engine
      await startTask?.value
      await applyTask?.value
      await engine.stop()
    }
  }

  // MARK: - Configuration Management

  func applyConfig() {
    guard !isLoadingPreferences else { return }
    if case .error = status {
      // Cancel any pending apply before restarting — it would run against a just-restarted
      // engine and is already superseded by the full startEngine sequence.
      applyConfigTask?.cancel()
      applyConfigTask = nil
      startEngine()
      return
    }
    // Cancel any pending apply and debounce by scheduling a new one
    applyConfigTask?.cancel()
    applyConfigTask = Task {
      // Small debounce delay to coalesce rapid property changes
      try? await Task.sleep(nanoseconds: 50_000_000)
      guard !Task.isCancelled else { return }
      await applyConfigAsync()
    }
  }

  func applyConfigAsync() async {
    guard isRunning && !isBusy && !isLoadingPreferences else { return }
    savePipelineStages()

    let config = buildConfigDict()
    // .sortedKeys ensures the serialized string is deterministic regardless of
    // Swift dictionary iteration order, so identical configs produce identical strings.
    let data = try? JSONSerialization.data(withJSONObject: config, options: .sortedKeys)
    let json = data.flatMap { String(data: $0, encoding: .utf8) } ?? ""
    if json == lastAppliedConfigYAML { return }

    // Capture a safe fallback status — never restore .applyingConfig (would be a stuck state)
    let fallbackStatus: AppStatus = (status == .applyingConfig) ? .running : status
    status = .applyingConfig

    // Remember what subscriptions were active so we can restart them after.
    let hadVuSubscription = isVuSubscriptionActive || vuSubscriptionTask != nil
    let hadStateSubscription = isStateSubscriptionActive || stateSubscriptionTask != nil

    do {
      // Prime faders before config change to avoid transition spikes
      await engine.setFaderMute(fader: 0, mute: isMuted)
      await engine.setFaderExternalVolume(fader: 0, db: volume)

      try await startEngineWithConfig(config)
      lastAppliedConfigYAML = json

      if await verifyEngineRunning(config: config) {
        status = .running
      } else {
        let msg = "Engine failed to reach running state"
        lastError = msg
        status = .error(msg)
      }
    } catch {
      print("[AppState] Config apply failed: \(error)")
      lastError = "Config error: \(error.localizedDescription)"
      lastAppliedConfigYAML = nil
      status = fallbackStatus
    }

    scheduleSpectrumRestart()

    // Restart subscriptions after config change. CamillaDSP disconnects all
    // WebSocket connections when it restarts the pipeline, so old subscription
    // tasks will have ended by now. Starting fresh avoids the error/restart race.
    if status == .running {
      if hadVuSubscription { startVuSubscription() }
      if hadStateSubscription { startStateSubscription() }
    }
  }

  @discardableResult
  private func verifyEngineRunning(config: [String: Any]) async -> Bool {
    for _ in 0..<25 {
      guard !Task.isCancelled else { return false }
      let state: String? = try? await engine.sendCommand("GetState")
      if state == "Running" || state == "Starting" || state == "Stalled" { return true }
      try? await Task.sleep(nanoseconds: 20_000_000)
    }

    guard !Task.isCancelled else { return false }
    print("[AppState] Engine state after config still not Running/Starting, retrying...")

    do {
      try await startEngineWithConfig(config)
      for _ in 0..<10 {
        guard !Task.isCancelled else { return false }
        try? await Task.sleep(nanoseconds: 100_000_000)
        let newState: String? = try? await engine.sendCommand("GetState")
        if newState == "Running" || newState == "Starting" || newState == "Stalled" { return true }
      }
      return false
    } catch {
      return false
    }
  }

  // MARK: - Controls

  func setVolume(_ db: Double) {
    volume = db
    // Use regular setVolume for live updates (provides smooth interpolation)
    Task { await engine.setVolume(db) }
  }
  func toggleMute() {
    isMuted.toggle()
    // Use regular setMute for live updates
    Task { await engine.setMute(isMuted) }
  }

  // MARK: - Private Helpers

  private func startEngineWithConfig(_ config: [String: Any]) async throws {
    let data = try JSONSerialization.data(withJSONObject: config)
    guard let json = String(data: data, encoding: .utf8) else {
      throw AudioBackendError.commandFailed("Failed to serialize config to JSON string")
    }
    try await engine.start(configJson: json)
  }

  func buildConfigDict() -> [String: Any] {
    var devices: [String: Any] = [
      "samplerate": playbackSampleRate,
      "chunksize": chunkSize,
      "volume_ramp_time": 200.0,  // ms
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
        devices["resampler"] = [
          "type": "AsyncPoly", "interpolation": resamplerInterpolation.rawValue,
        ]
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
