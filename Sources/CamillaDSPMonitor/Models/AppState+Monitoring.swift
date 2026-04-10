// AppState+Monitoring - Level polling, audio capture, and spectrum management

import AppKit
import CamillaDSPLib
import Foundation

extension AppState {

  func recreateSpectrumAnalyzer() {
    spectrumAnalyzer = FFTSpectrumAnalyzer(sampleRate: sampleRate, chunkSize: chunkSize)
  }

  func scheduleSpectrumRestart() {
    guard isRunning else { return }
    spectrumRestartTask?.cancel()
    spectrumRestartTask = Task {
      try? await Task.sleep(nanoseconds: 500_000_000)
      guard !Task.isCancelled else { return }
      recreateSpectrumAnalyzer()
      if audioTap == nil {
        let ref = analyzerRef
        audioTap = CoreAudioTap(onAudio: { waveform in
          ref.analyzer?.enqueueAudio(waveform)
        })
      }
      await audioTap?.start(deviceName: selectedCaptureDevice)
    }
  }

  func startMonitoringTimer() {
    monitoringTask?.cancel()
    vuSubscriptionTask?.cancel()
    stateSubscriptionTask?.cancel()
    isVuSubscriptionActive = false
    isStateSubscriptionActive = false

    // Try to start VU subscription. Will be managed (started/stopped) by the
    // polling loop based on whether the app is active.
    startVuSubscription()

    // Try state subscription (server-push) — lightweight, always on
    startStateSubscription()

    // Polling loop — adapts based on which subscriptions are active.
    // When the app goes to background, stops VU subscription to save CPU.
    // When it comes back to foreground, restarts VU subscription.
    monitoringTask = Task {
      try? await Task.sleep(nanoseconds: 300_000_000)
      var wasActive = true
      while !Task.isCancelled {
        let isActive = NSApp?.isActive ?? false

        // Manage VU subscription lifecycle based on app active state
        if isActive && !wasActive && !isVuSubscriptionActive {
          startVuSubscription()
        } else if !isActive && wasActive && isVuSubscriptionActive {
          vuSubscriptionTask?.cancel()
          vuSubscriptionTask = nil
          isVuSubscriptionActive = false
          print("[AppState] VU subscription paused (app inactive)")
        }
        wasActive = isActive

        if status == .running {
          if isVuSubscriptionActive && isStateSubscriptionActive {
            await pollLoadOnly()
          } else if isVuSubscriptionActive {
            await pollStateAndLoad()
          } else {
            await pollLevels()
          }
        }
        if isVuSubscriptionActive {
          let interval: UInt64 = isActive ? 500_000_000 : 1_000_000_000
          try? await Task.sleep(nanoseconds: interval)
        } else {
          let interval: UInt64 = isActive ? 100_000_000 : 500_000_000
          try? await Task.sleep(nanoseconds: interval)
        }
      }
    }
  }

  func startStateSubscription() {
    stateSubscriptionTask?.cancel()
    stateSubscriptionTask = Task {
      guard let stream = await engine.subscribeState() else {
        print("[AppState] State subscription not available, using polling")
        return
      }
      isStateSubscriptionActive = true
      print("[AppState] State subscription started")
      for await update in stream {
        guard !Task.isCancelled else { break }
        handleStateUpdate(state: update.state, stopReason: update.stopReason)
      }
      isStateSubscriptionActive = false
    }
  }

  func startVuSubscription() {
    vuSubscriptionTask?.cancel()
    vuSubscriptionTask = Task {
      guard
        let stream = await engine.subscribeVuLevels(
          maxRate: 10.0, attack: 5.0, release: 100.0
        )
      else {
        print("[AppState] VU subscription not available, using polling")
        return
      }
      isVuSubscriptionActive = true
      print("[AppState] VU subscription started")
      for await vu in stream {
        guard !Task.isCancelled else { break }
        levels.update(
          capturePeak: StereoLevel(from: vu.capture_peak),
          captureRms: StereoLevel(from: vu.capture_rms),
          playbackPeak: StereoLevel(from: vu.playback_peak),
          playbackRms: StereoLevel(from: vu.playback_rms)
        )
        let bands = spectrumAnalyzer?.readBands() ?? spectrum.bands
        spectrum.update(bands: bands)
      }
      isVuSubscriptionActive = false
    }
  }

  func stopMonitoring() {
    monitoringTask?.cancel()
    monitoringTask = nil
    vuSubscriptionTask?.cancel()
    vuSubscriptionTask = nil
    stateSubscriptionTask?.cancel()
    stateSubscriptionTask = nil
    isVuSubscriptionActive = false
    isStateSubscriptionActive = false
    spectrumRestartTask?.cancel()
    spectrumRestartTask = nil
    isPollingLevels = false
    stopAudioCapture()
    spectrumAnalyzer = nil
  }

  // MARK: - State Change Handling

  /// Handle a state update from either subscription or polling.
  private func handleStateUpdate(state: String, stopReason: String?) {
    if state == "Running" || state == "Starting" || state == "Stalled" {
      return  // Healthy states — nothing to do
    }
    // Don't trigger recovery while we're already busy (starting or applying config).
    // CamillaDSP briefly reports Inactive during config transitions.
    guard !isBusy else { return }
    // Engine stopped unexpectedly — attempt recovery
    let reason = stopReason ?? "None"
    if reason == "None" { return }
    let now = Date()
    if let last = lastRecoveryTime, now.timeIntervalSince(last) < 5.0 { return }
    lastRecoveryTime = now
    lastAppliedConfigYAML = nil
    // Use applyConfig() instead of applyConfigAsync() so that if status is .error,
    // it takes the startEngine() path for full recovery.
    applyConfig()
  }

  // MARK: - Polling Variants

  /// Minimal poll: only processing load. Used when both subscriptions are active.
  private func pollLoadOnly() async {
    pollCounter += 1
    if pollCounter % 2 == 0 {
      let pLoad = await engine.fetchProcessingLoad()
      load.update(load: Double(pLoad))
    }
  }

  /// State + load poll. Used when VU subscription is active but state subscription isn't.
  private func pollStateAndLoad() async {
    do {
      let state: String? = try await engine.sendCommand("GetState")

      if state != "Running" && state != "Starting" && state != "Stalled" {
        try? await Task.sleep(nanoseconds: 500_000_000)
        let secondCheck: String? = try? await engine.sendCommand("GetState")
        if secondCheck == "Running" || secondCheck == "Starting" || secondCheck == "Stalled" {
          return
        }
        let stopReason = await engine.getStopReason() ?? "None"
        handleStateUpdate(state: state ?? "Unknown", stopReason: stopReason)
        return
      }

      pollCounter += 1
      if pollCounter % 2 == 0 {
        let pLoad = await engine.fetchProcessingLoad()
        load.update(load: Double(pLoad))
      }
    } catch {
      if !(await engine.ping()) {
        status = .error("Connection lost: \(error.localizedDescription)")
        stopMonitoring()
      }
    }
  }

  /// Full polling. Used when no subscriptions are available.
  private func pollLevels() async {
    guard status == .running && !isPollingLevels else { return }
    isPollingLevels = true
    defer { isPollingLevels = false }

    do {
      let state: String? = try await engine.sendCommand("GetState")

      if state != "Running" && state != "Starting" && state != "Stalled" {
        try? await Task.sleep(nanoseconds: 500_000_000)
        let secondCheck: String? = try? await engine.sendCommand("GetState")
        if secondCheck == "Running" || secondCheck == "Starting" || secondCheck == "Stalled" {
          return
        }
        let stopReason = await engine.getStopReason() ?? "None"
        handleStateUpdate(state: state ?? "Unknown", stopReason: stopReason)
        return
      }

      if state == "Starting" { return }
      pollCounter += 1
      let includeLoad = pollCounter % 5 == 0
      guard let sigLevels = await engine.getSignalLevels(includeLoad: includeLoad) else { return }

      levels.update(
        capturePeak: StereoLevel(from: sigLevels.capture_peak),
        captureRms: StereoLevel(from: sigLevels.capture_rms),
        playbackPeak: StereoLevel(from: sigLevels.playback_peak),
        playbackRms: StereoLevel(from: sigLevels.playback_rms)
      )
      let bands = spectrumAnalyzer?.readBands() ?? spectrum.bands
      spectrum.update(bands: bands)
      if includeLoad {
        load.update(load: Double(sigLevels.processing_load ?? 0))
      }

    } catch {
      if !(await engine.ping()) {
        status = .error("Connection lost: \(error.localizedDescription)")
        stopMonitoring()
      }
    }
  }

  func startAudioCapture() {
    if audioTap == nil {
      let ref = analyzerRef
      audioTap = CoreAudioTap(onAudio: { waveform in
        ref.analyzer?.enqueueAudio(waveform)
      })
    }
    let tap = audioTap
    let deviceName = selectedCaptureDevice
    Task { await tap?.start(deviceName: deviceName) }
  }

  func stopAudioCapture() {
    let tap = audioTap
    Task { await tap?.stop() }
  }
}
