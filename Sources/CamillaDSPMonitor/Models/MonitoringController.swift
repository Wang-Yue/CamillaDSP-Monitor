// MonitoringController - State and VU level subscriptions

import AppKit
import CamillaDSPLib
import Foundation
import Observation

@MainActor
@Observable
final class MonitoringController {
  let engine: DSPEngine
  let levels: LevelState
  let devices: AudioDeviceManager
  let settings: AudioSettings
  var spectrum: SpectrumEngine?

  /// Fired with the new AppStatus whenever CamillaDSP reports a state change.
  var onStatusChange: ((AppStatus) -> Void)?
  /// Fired when a CaptureFormatChange stop reason requires restarting the engine.
  var onRestartEngine: (() -> Void)?

  /// Current status to avoid late VU updates during inactive state.
  private var currentStatus: AppStatus = .inactive
  private var pollingTimer: Timer?

  // MARK: - Init

  init(
    engine: DSPEngine, levels: LevelState,
    devices: AudioDeviceManager, settings: AudioSettings
  ) {
    self.engine = engine
    self.levels = levels
    self.devices = devices
    self.settings = settings
  }

  // MARK: - Polling

  /// Start polling state and VU levels.
  func startSubscriptions() {
    pollingTimer?.invalidate()
    pollingTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
      guard let self else { return }
      Task { @MainActor in
        await self.poll()
      }
    }
  }

  private func poll() async {
    // 1. Poll Status
    if let update = await engine.getStatus() {
      handleStateUpdate(
        state: update.state,
        stopReason: update.stopReason,
        stopReasonRate: update.stopReasonRate
      )
    }

    // 2. Poll VU Levels
    if currentStatus != .inactive, let vu = await engine.getVuLevels() {
      levels.update(
        capturePeak: StereoLevel(from: vu.capture_peak),
        captureRms: StereoLevel(from: vu.capture_rms),
        playbackPeak: StereoLevel(from: vu.playback_peak),
        playbackRms: StereoLevel(from: vu.playback_rms)
      )
    } else if currentStatus == .inactive {
      levels.reset()
    }

    // 3. Poll Spectrum Bands
    if currentStatus == .running, let spectrum, spectrum.visibilityCount > 0 {
      if let bands = await engine.getSpectrumBands(), !bands.isEmpty {
        spectrum.updateBands(bands)
      } else {
        spectrum.reset()
      }
    } else {
      spectrum?.reset()
    }
  }

  // MARK: - State Change Handling

  private func handleStateUpdate(state: String, stopReason: String?, stopReasonRate: Int? = nil) {
    let newStatus: AppStatus?
    switch state {
    case "RUNNING": newStatus = .running
    case "PAUSED": newStatus = .paused
    case "STARTING": newStatus = .starting
    case "STALLED": newStatus = .stalled
    case "INACTIVE": newStatus = .inactive
    default: newStatus = nil
    }

    if let newStatus, newStatus != currentStatus {
      currentStatus = newStatus
      onStatusChange?(newStatus)
      if newStatus == .inactive {
        levels.reset()
      }
    }

    let reason = stopReason ?? "None"

    if reason == "CaptureFormatChange", let newRate = stopReasonRate {
      print("[MonitoringController] Capture format change detected, switching to \(newRate) Hz")
      if settings.resamplerEnabled {
        devices.captureConfig.sampleRate = newRate
      } else {
        devices.playbackConfig.sampleRate = newRate
      }
      onRestartEngine?()
    }
  }
}
