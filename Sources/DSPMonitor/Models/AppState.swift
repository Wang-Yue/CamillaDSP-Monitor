// AppState - Thin coordinator that owns and wires all domain objects

import DSPLib
import Foundation
import Observation
import SwiftUI

@MainActor
@Observable
final class AppState {
  let settings: AudioSettings
  let pipeline: PipelineStore
  let devices: AudioDeviceManager
  let monitoring: MonitoringController
  let dsp: DSPEngineController
  let logManager = LogManager()

  init() {
    print("[AppState] Initializing...")

    let engine = DSPEngine()
    let settings = AudioSettings()
    let pipeline = PipelineStore()
    let devices = AudioDeviceManager(engine: engine, settings: settings)

    let monitoring = MonitoringController(
      engine: engine,
      devices: devices, settings: settings)

    let dsp = DSPEngineController(
      engine: engine, devices: devices, settings: settings, pipeline: pipeline,
      monitoring: monitoring)
    logManager.setEngine(engine)

    self.settings = settings
    self.pipeline = pipeline
    self.devices = devices
    self.monitoring = monitoring
    self.dsp = dsp

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
