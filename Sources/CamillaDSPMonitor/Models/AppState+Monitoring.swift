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

    // Start VU subscription at the rate appropriate for the current active state.
    // Rate is adjusted (by restarting the subscription) whenever active state changes.
    let initiallyActive = NSApp?.isActive ?? true
    startVuSubscription(maxRate: initiallyActive ? 10.0 : 2.0)

    // Try state subscription (server-push) — lightweight, always on
    startStateSubscription()

    // Polling loop — adapts based on which subscriptions are active.
    // VU subscription stays active in both foreground and background; only the
    // max_rate differs (20 Hz active, 2 Hz background).
    monitoringTask = Task {
      try? await Task.sleep(nanoseconds: 300_000_000)
      var wasActive = initiallyActive
      while !Task.isCancelled {
        let isActive = NSApp?.isActive ?? false

        // Restart VU subscription with the appropriate rate on active-state transitions
        if isActive != wasActive {
          startVuSubscription(maxRate: isActive ? 10.0 : 2.0)
          wasActive = isActive
        }

        try? await Task.sleep(nanoseconds: isActive ? 500_000_000 : 1_000_000_000)
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

  func startVuSubscription(maxRate: Float = 10.0) {
    vuSubscriptionTask?.cancel()
    vuSubscriptionTask = Task {
      guard
        let stream = await engine.subscribeVuLevels(
          maxRate: maxRate, attack: 5.0, release: 100.0
        )
      else {
        print("[AppState] VU subscription not available, using polling")
        return
      }
      isVuSubscriptionActive = true
      print("[AppState] VU subscription started (maxRate: \(maxRate))")
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
