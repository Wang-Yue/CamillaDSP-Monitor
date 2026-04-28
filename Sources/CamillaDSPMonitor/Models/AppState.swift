// AppState - Thin coordinator that owns and wires all domain objects

import CamillaDSPLib
import Foundation
import Observation
import SwiftUI

@MainActor
@Observable
final class AppState {
  var isMiniPlayerActive = false

  let settings: AudioSettings
  let pipeline: PipelineStore
  let devices: AudioDeviceManager
  let monitoring: MonitoringController
  let dsp: DSPEngineController
  let spectrum: SpectrumEngine
  let spectroscope: SpectrogramEngine
  let levels: LevelState
  let vuSettings = VUSettings()  // Added persistent VU settings
  let logManager = LogManager()

  var showLevelMetersInDashboard = true {
    didSet {
      UserDefaults.standard.set(showLevelMetersInDashboard, forKey: "show_levels_in_dashboard")
    }
  }
  var showSpectrumInDashboard = true {
    didSet {
      UserDefaults.standard.set(showSpectrumInDashboard, forKey: "show_spectrum_in_dashboard")
    }
  }
  var showSpectrogramInDashboard = true {
    didSet {
      UserDefaults.standard.set(showSpectrogramInDashboard, forKey: "show_spectrogram_in_dashboard")
    }
  }
  var showAnalogVUInDashboard = true {
    didSet {
      UserDefaults.standard.set(showAnalogVUInDashboard, forKey: "show_analog_vu_in_dashboard")
    }
  }

  init() {
    print("[AppState] Initializing...")

    self.showLevelMetersInDashboard =
      UserDefaults.standard.object(forKey: "show_levels_in_dashboard") != nil
      ? UserDefaults.standard.bool(forKey: "show_levels_in_dashboard") : true
    self.showSpectrumInDashboard =
      UserDefaults.standard.object(forKey: "show_spectrum_in_dashboard") != nil
      ? UserDefaults.standard.bool(forKey: "show_spectrum_in_dashboard") : true
    self.showSpectrogramInDashboard =
      UserDefaults.standard.object(forKey: "show_spectrogram_in_dashboard") != nil
      ? UserDefaults.standard.bool(forKey: "show_spectrogram_in_dashboard") : true
    self.showAnalogVUInDashboard =
      UserDefaults.standard.object(forKey: "show_analog_vu_in_dashboard") != nil
      ? UserDefaults.standard.bool(forKey: "show_analog_vu_in_dashboard") : true

    let engine = DSPEngine()
    let settings = AudioSettings()
    let pipeline = PipelineStore()
    let levels = LevelState()
    let devices = AudioDeviceManager(engine: engine, settings: settings)
    let spectrum = SpectrumEngine()
    let spectroscope = SpectrogramEngine()

    let monitoring = MonitoringController(
      engine: engine, levels: levels, spectrum: spectrum,
      spectroscope: spectroscope,
      devices: devices, settings: settings)

    let dsp = DSPEngineController(
      engine: engine, devices: devices, settings: settings, pipeline: pipeline,
      monitoring: monitoring, levels: levels)
    logManager.setEngine(engine)

    self.settings = settings
    self.pipeline = pipeline
    self.devices = devices
    self.monitoring = monitoring
    self.dsp = dsp
    self.levels = levels
    self.spectrum = spectrum
    self.spectroscope = spectroscope

    // Load persisted preferences.
    settings.loadPreferences()

    // Wire callbacks after all objects exist.
    settings.onChanged = { [weak devices, weak dsp] in
      devices?.validateSampleRates()
      dsp?.applyConfig()
    }
    devices.onConfigChanged = { [weak dsp] in
      dsp?.applyConfig()
    }
    pipeline.onChanged = { [weak dsp] in
      dsp?.applyConfig()
    }
    pipeline.eqPresets = pipeline.loadEQPresets()
    pipeline.createDefaultEQPresetsIfNeeded()
    pipeline.loadPipelineStages()

    Task {
      await devices.fetchDevices()  // internally calls refreshDeviceCapabilities()
      devices.validateSampleRates()
    }
  }

  // MARK: - AppDelegate forwarding

  func savePipelineStages() { pipeline.savePipelineStages() }
  func saveEQPresets() { pipeline.saveEQPresets() }
}
