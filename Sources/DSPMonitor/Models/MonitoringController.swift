// MonitoringController - State and VU level subscriptions

import AppKit
import DSPConfig
import DSPLib
import Foundation
import Observation

@MainActor
@Observable
final class MonitoringController {
  let engine: DSPEngine
  let devices: AudioDeviceManager
  let settings: AudioSettings

  var pollingRate: Double = 10.0 {
    didSet {
      UserDefaults.standard.set(pollingRate, forKey: "pollingRate")
    }
  }

  /// Fired with the new ProcessingState whenever DSP engine reports a state change.
  var onStatusChange: ((ProcessingState) -> Void)?
  /// Fired when a CaptureFormatChange stop reason requires restarting the engine.
  var onRestartEngine: (() -> Void)?

  /// Current status to avoid late VU updates during inactive state.
  private var currentStatus: ProcessingState = .inactive
  private var pollingTask: Task<Void, Never>?

  // MARK: - Init

  init(
    engine: DSPEngine,
    devices: AudioDeviceManager, settings: AudioSettings
  ) {
    self.engine = engine
    self.devices = devices
    self.settings = settings

    let savedPollingRate = UserDefaults.standard.double(forKey: "pollingRate")
    self.pollingRate = savedPollingRate > 0 ? savedPollingRate : 10.0

    pollingTask = Task { @MainActor [weak self] in
      while !Task.isCancelled {
        guard let self else { break }
        await self.poll()
        try? await Task.sleep(for: .seconds(1.0 / self.pollingRate))
      }
    }
  }

  private func poll() async {
    // Poll Status
    let update = await engine.getStatus()
    handleStateUpdate(
      state: update.state,
      stopReason: update.stopReason
    )
  }

  // MARK: - State Change Handling

  private func handleStateUpdate(state: ProcessingState, stopReason: ProcessingStopReason) {
    if state != currentStatus {
      currentStatus = state
      onStatusChange?(state)
    }

    switch stopReason {
    case .none:
      break
    case .done:
      break
    case .captureError(let message):
      print("[MonitoringController] Capture error: \(message)")
    case .playbackError(let message):
      print("[MonitoringController] Playback error: \(message)")
    case .captureFormatChange(let newRate):
      print("[MonitoringController] Capture format change detected, switching to \(newRate) Hz")
      if settings.resamplerEnabled {
        devices.captureConfig.sampleRate = newRate
      } else {
        // When the resampler is disabled, capture rate will follow the playback rate.
        // So change the playback rate to the new capture rate.
        devices.playbackConfig.sampleRate = newRate
      }
      onRestartEngine?()
    case .playbackFormatChange(let newRate):
      print("[MonitoringController] Playback format change detected, switching to \(newRate) Hz")
      devices.playbackConfig.sampleRate = newRate
      onRestartEngine?()
    case .unknownError(let message):
      print("[MonitoringController] Unknown error: \(message)")
    }
  }
}
