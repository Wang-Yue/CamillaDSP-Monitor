// MonitoringController - State and VU level subscriptions

import AppKit
import CamillaDSPLib
import Foundation

@MainActor
final class MonitoringController: ObservableObject {
  let engine: DSPEngine
  let levels: LevelState
  let devices: AudioDeviceManager
  let settings: AudioSettings

  /// Fired with the new AppStatus whenever CamillaDSP reports a state change.
  var onStatusChange: ((AppStatus) -> Void)?
  /// Fired when a CaptureFormatChange stop reason requires restarting the engine.
  var onRestartEngine: (() -> Void)?

  /// Current status to avoid late VU updates during inactive state.
  private var currentStatus: AppStatus = .inactive

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

  // MARK: - Subscriptions

  /// Start both subscriptions once after the WebSocket connects.
  /// They run for the lifetime of the app — never cancelled or restarted.
  func startSubscriptions() {
    Task {
      guard let stream = await engine.subscribeState() else {
        print("[MonitoringController] State subscription not available")
        return
      }
      print("[MonitoringController] State subscription started")
      for await update in stream {
        handleStateUpdate(
          state: update.state,
          stopReason: update.stopReason,
          stopReasonRate: update.stopReasonRate
        )
      }
      print("[MonitoringController] State subscription ended")
    }

    Task {
      guard let stream = await engine.subscribeVuLevels(maxRate: 10.0, attack: 5.0, release: 100.0)
      else {
        print("[MonitoringController] VU subscription not available")
        return
      }
      print("[MonitoringController] VU subscription started")
      for await vu in stream {
        if currentStatus == .inactive {
          levels.reset()
          continue 
        }
        levels.update(
          capturePeak: StereoLevel(from: vu.capture_peak),
          captureRms: StereoLevel(from: vu.capture_rms),
          playbackPeak: StereoLevel(from: vu.playback_peak),
          playbackRms: StereoLevel(from: vu.playback_rms)
        )
      }
      print("[MonitoringController] VU subscription ended")
    }
  }

  // MARK: - State Change Handling

  private func handleStateUpdate(state: String, stopReason: String?, stopReasonRate: Int? = nil) {
    let newStatus: AppStatus?
    switch state {
    case "Running": newStatus = .running
    case "Paused": newStatus = .paused
    case "Starting": newStatus = .starting
    case "Stalled": newStatus = .stalled
    case "Inactive": newStatus = .inactive
    default: newStatus = nil
    }

    if let newStatus {
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
