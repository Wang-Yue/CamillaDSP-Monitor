// AppState+Monitoring - Level polling, audio capture, and spectrum management

import AppKit
import CamillaDSPLib
import Foundation

extension AppState {

  func recreateSpectrumAnalyzer() {
    spectrumAnalyzer = FFTSpectrumAnalyzer(sampleRate: sampleRate, chunkSize: chunkSize)
    // Pause immediately if no spectrum view is currently visible — avoids wasted FFT
    // computation in the background when the user is on a non-spectrum tab or the
    // mini player is in pipeline/meters mode.
    if spectrumViewCount == 0 { spectrumAnalyzer?.pause() }
  }

  func scheduleSpectrumRestart() {
    guard status == .running else { return }
    spectrumRestartTask?.cancel()
    spectrumRestartTask = Task {
      try? await Task.sleep(nanoseconds: 500_000_000)
      guard !Task.isCancelled else { return }

      if spectrumAnalyzer == nil {
        recreateSpectrumAnalyzer()
      }

      if audioTap == nil {
        let ref = analyzerRef
        audioTap = CoreAudioTap(onAudio: { waveform in
          ref.analyzer?.enqueueAudio(waveform)
        })
      }

      // Only restart the audio tap when the capture device changed or the tap stopped.
      // Restarting AVAudioEngine (stopSync + start) for every config change (e.g. stage
      // toggle) causes a CoreAudio reconfiguration spike even when the device is the same.
      let device = captureConfig.deviceName
      let tapRunning = await audioTap?.isRunning ?? false
      if !tapRunning || device != audioTapDeviceName {
        audioTapDeviceName = device
        await audioTap?.start(deviceName: device)
      }
    }
  }

  func startMonitoringTimer() {
    monitoringTask?.cancel()
    vuSubscriptionTask?.cancel()
    isVuSubscriptionActive = false

    // Start VU subscription at the rate appropriate for the current active state.
    // Rate is adjusted (by restarting the subscription) whenever active state changes.
    let initiallyActive = NSApp?.isActive ?? true
    startVuSubscription(maxRate: initiallyActive ? 10.0 : 2.0)

    // Polling loop — adapts based on which subscriptions are active.
    // VU subscription stays active in both foreground and background; only the
    // max_rate differs (20 Hz active, 2 Hz background).
    monitoringTask = Task {
      try? await Task.sleep(nanoseconds: 300_000_000)
      guard !Task.isCancelled else { return }
      var wasActive = initiallyActive
      while !Task.isCancelled {
        let isActive = NSApp?.isActive ?? false

        // Restart VU subscription with the appropriate rate on active-state transitions
        if isActive != wasActive {
          startVuSubscription(maxRate: isActive ? 10.0 : 2.0)
          wasActive = isActive
        }

        try? await Task.sleep(nanoseconds: isActive ? 500_000_000 : 1_000_000_000)
        guard !Task.isCancelled else { return }
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
      print("[AppState] State subscription started")
      for await update in stream {
        guard !Task.isCancelled else { break }
        handleStateUpdate(
          state: update.state,
          stopReason: update.stopReason,
          stopReasonRate: update.stopReasonRate
        )
      }
      // If the loop ends, we likely lost connection
      print("[AppState] State subscription ended")
    }
  }

  func startVuSubscription(maxRate: Float = 10.0) {
    vuSubscriptionTask?.cancel()
    isVuSubscriptionActive = false
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
    isVuSubscriptionActive = false
    spectrumRestartTask?.cancel()
    spectrumRestartTask = nil
    stopAudioCapture()
    spectrumAnalyzer = nil
  }

  // MARK: - State Change Handling

  /// Handle a state update from either subscription or polling.
  private func handleStateUpdate(state: String, stopReason: String?, stopReasonRate: Int? = nil) {
    // Map CamillaDSP states to AppStatus
    let newStatus: AppStatus
    switch state {
    case "Running":
      newStatus = .running
    case "Paused":
      newStatus = .paused
    case "Starting":
      newStatus = .starting
    case "Stalled":
      newStatus = .stalled
    case "Inactive":
      newStatus = .inactive
    default:
      newStatus = status // Keep current if unknown
    }

    if newStatus != status {
      status = newStatus
    }

    let reason = stopReason ?? "None"
    
    // Handle format changes by updating config first.
    if reason == "CaptureFormatChange", let newRate = stopReasonRate {
      print("[AppState] Capture format change detected, switching to \(newRate) Hz")
      if resamplerEnabled {
        captureConfig.sampleRate = newRate
      } else {
        // When resampler is disabled, capture and playback rates must match.
        // Update playbackConfig so validateSampleRates() syncs capture via its didSet.
        playbackConfig.sampleRate = newRate
      }
      startEngine()
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
    let deviceName = captureConfig.deviceName
    audioTapDeviceName = deviceName
    Task { await tap?.start(deviceName: deviceName) }
  }

  func stopAudioCapture() {
    let tap = audioTap
    audioTapDeviceName = nil
    Task { await tap?.stop() }
  }
}
