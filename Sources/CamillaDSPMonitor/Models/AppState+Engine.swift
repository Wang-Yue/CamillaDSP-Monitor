// AppState+Engine - DSP engine control and config building

import CamillaDSPLib
import SwiftUI

extension AppState {

  // MARK: - Engine Lifecycle

  func startEngine() {
    if status == .running {
      return
    }

    guard devicesAvailable() else {
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
        let config = buildConfigDict()
        try await startEngineWithConfig(config)
        guard !Task.isCancelled else { return }

        // Status will be updated by handleStateUpdate via state subscription.
        startMonitoringTimer()
        scheduleSpectrumRestart()
      } catch {
        guard !Task.isCancelled else { return }
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
    
    stopMonitoring()
    levels.reset()
    spectrum.reset()
    load.reset()
    Task {
      // Await cancelled tasks to ensure they've stopped before we stop the engine
      await startTask?.value
      await applyTask?.value
      await engine.stop()
    }
  }

  // MARK: - Configuration Management

  func applyConfig() {
    if status != .running {
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
    savePipelineStages()

    // Remember what subscriptions were active so we can restart them after.
    let hadVuSubscription = isVuSubscriptionActive || vuSubscriptionTask != nil

    do {
      // Prime faders before config change to avoid transition spikes
      await engine.setFaderMute(fader: 0, mute: isMuted)
      await engine.setFaderExternalVolume(fader: 0, db: volume)

      let config = buildConfigDict()
      try await startEngineWithConfig(config)
      // Success — state subscription will move status accordingly.
    } catch {
      print("[AppState] Config apply failed: \(error)")
    }

    scheduleSpectrumRestart()

    // Restart subscriptions after config change. CamillaDSP disconnects all
    // WebSocket connections when it restarts the pipeline, so old subscription
    // tasks will have ended by now. Starting fresh avoids the error/restart race.
    if hadVuSubscription { startVuSubscription() }
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
      "samplerate": playbackConfig.sampleRate,
      "chunksize": chunkSize,
      "volume_ramp_time": 200.0,  // ms
      "capture": [
        "type": "CoreAudio", "channels": captureConfig.channels,
        "device": captureConfig.deviceName as Any,
        "format": captureConfig.format,
      ],
      "playback": [
        "type": "CoreAudio", "channels": playbackConfig.channels,
        "device": playbackConfig.deviceName as Any,
        "format": playbackConfig.format, "exclusive": exclusiveMode,
      ],
    ]

    if resamplerEnabled {
      devices["capture_samplerate"] = captureConfig.sampleRate
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
