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
  let spectrum: SpectrumEngine
  let spectroscope: SpectrogramEngine

  var pollingRate: Double = 10.0 {
    didSet {
      UserDefaults.standard.set(pollingRate, forKey: "pollingRate")
    }
  }

  /// Fired with the new ProcessingState whenever CamillaDSP reports a state change.
  var onStatusChange: ((ProcessingState) -> Void)?
  /// Fired when a CaptureFormatChange stop reason requires restarting the engine.
  var onRestartEngine: (() -> Void)?

  /// Current status to avoid late VU updates during inactive state.
  private var currentStatus: ProcessingState = .inactive
  private var pollingTask: Task<Void, Never>?

  // MARK: - Init

  init(
    engine: DSPEngine, levels: LevelState, spectrum: SpectrumEngine,
    spectroscope: SpectrogramEngine,
    devices: AudioDeviceManager, settings: AudioSettings
  ) {
    self.engine = engine
    self.levels = levels
    self.spectrum = spectrum
    self.spectroscope = spectroscope
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
    // 1. Poll Status
    let update = await engine.getStatus()
    handleStateUpdate(
      state: update.state,
      stopReason: update.stopReason
    )

    // 2. Poll VU Levels
    if currentStatus != .inactive, currentStatus != .paused, levels.visibilityCount > 0 {
      let vu = await engine.getVuLevels()
      levels.update(
        capturePeak: StereoLevel(from: vu.capture_peak),
        captureRms: StereoLevel(from: vu.capture_rms),
        playbackPeak: StereoLevel(from: vu.playback_peak),
        playbackRms: StereoLevel(from: vu.playback_rms)
      )
    } else {
      levels.reset()
    }

    // 3. Poll Spectrum Bands
    if currentStatus != .inactive, currentStatus != .paused, spectrum.visibilityCount > 0,
      let spectrumData = await fetchSpectrum(for: spectrum)
    {
      spectrum.updateSpectrum(
        frequencies: spectrumData.frequencies, magnitudes: spectrumData.magnitudes)
    } else {
      spectrum.reset()
    }

    // 4. Poll Spectroscope Bands
    if currentStatus != .inactive, currentStatus != .paused, spectroscope.visibilityCount > 0,
      let spectrumData = await fetchSpectroscope(for: spectroscope)
    {
      spectroscope.updateSpectrum(
        frequencies: spectrumData.frequencies, magnitudes: spectrumData.magnitudes)
    } else {
      spectroscope.reset()
    }
  }

  private func fetchSpectrum(for spectrum: SpectrumEngine) async -> Spectrum? {
    do {
      return try await engine.getSpectrum(
        isCapture: spectrum.isCapture,
        channel: nil,
        minFreq: spectrum.minFreq,
        maxFreq: spectrum.maxFreq,
        nBins: spectrum.nBins
      )
    } catch {
      print("[MonitoringController] Failed to get spectrum: \(error.localizedDescription)")
      return nil
    }
  }

  private func fetchSpectroscope(for spectroscope: SpectrogramEngine) async -> Spectrum? {
    do {
      return try await engine.getSpectrum(
        isCapture: spectroscope.isCapture,
        channel: nil,
        minFreq: spectroscope.minFreq,
        maxFreq: spectroscope.maxFreq,
        nBins: spectroscope.nBins
      )
    } catch {
      print("[MonitoringController] Failed to get spectroscope: \(error.localizedDescription)")
      return nil
    }
  }

  // MARK: - State Change Handling

  private func handleStateUpdate(state: ProcessingState, stopReason: ProcessingStopReason) {
    if state != currentStatus {
      currentStatus = state
      onStatusChange?(state)
      if state == .inactive || state == .paused {
        levels.reset()
        spectrum.reset()
        spectroscope.reset()
      }
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
