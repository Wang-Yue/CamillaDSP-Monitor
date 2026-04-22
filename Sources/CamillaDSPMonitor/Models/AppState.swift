// AppState - Thin coordinator that owns and wires all domain objects

import CamillaDSPLib
import Foundation
import Observation
import SwiftUI

public enum AppStatus: Equatable, Sendable {
  case inactive
  case starting
  case running
  case paused
  case stalled
}

@MainActor
@Observable
final class AppState {
  var isMiniPlayerActive = false

  let engine: DSPEngine
  let settings: AudioSettings
  let pipeline: PipelineStore
  let devices: AudioDeviceManager
  let monitoring: MonitoringController
  let dsp: DSPEngineController
  let spectrum: SpectrumEngine
  let levels: LevelState
  let vuSettings = VUSettings()  // Added persistent VU settings
  let logManager = LogManager()

  init() {
    print("[AppState] Initializing...")
    DSPEngine.killStaleCamillaDSP()

    let engine = DSPEngine()
    let settings = AudioSettings()
    let pipeline = PipelineStore()
    let levels = LevelState()
    let devices = AudioDeviceManager(engine: engine, settings: settings)
    let monitoring = MonitoringController(
      engine: engine, levels: levels,
      devices: devices, settings: settings)
    let dsp = DSPEngineController(
      engine: engine, devices: devices, settings: settings, pipeline: pipeline,
      monitoring: monitoring, levels: levels)
    let spectrum = SpectrumEngine(dsp: dsp, devices: devices, settings: settings)

    self.engine = engine
    self.settings = settings
    self.pipeline = pipeline
    self.devices = devices
    self.monitoring = monitoring
    self.dsp = dsp
    self.spectrum = spectrum
    self.levels = levels

    // Wire callbacks after all objects exist.
    settings.onChanged = { [weak devices, weak dsp, weak spectrum] in
      devices?.validateSampleRates()
      dsp?.applyConfig()
      spectrum?.refresh()
    }
    devices.onConfigChanged = { [weak dsp, weak spectrum] in
      dsp?.applyConfig()
      spectrum?.refresh()
    }
    pipeline.onChanged = { [weak dsp] in
      dsp?.applyConfig()
    }

    monitoring.onStatusChange = { [weak dsp, weak spectrum] newStatus in
      guard let dsp, newStatus != dsp.status else { return }
      dsp.status = newStatus
      spectrum?.refresh()
    }
    monitoring.onRestartEngine = { [weak dsp] in
      dsp?.startEngine()
    }

    // Load persisted preferences.
    settings.loadPreferences()
    pipeline.eqPresets = pipeline.loadEQPresets()
    pipeline.createDefaultEQPresetsIfNeeded()
    pipeline.loadPipelineStages()

    Task {
      do {
        if settings.camillaDSPPath.isEmpty {
          let home = ProcessInfo.processInfo.environment["HOME"] ?? ""
          for path in [
            "\(home)/camilladsp/target/release/camilladsp",
            "\(home)/Downloads/camilladsp",
            "/usr/local/bin/camilladsp",
            "/opt/homebrew/bin/camilladsp",
          ] {
            if FileManager.default.fileExists(atPath: path) {
              settings.camillaDSPPath = path
              break
            }
          }
        }

        try await engine.connect(binaryPath: settings.camillaDSPPath)
        monitoring.startSubscriptions()
        await devices.fetchDevices()  // internally calls refreshDeviceCapabilities()
        devices.validateSampleRates()
      } catch {
        print("[AppState] Initial connection failed: \(error)")
      }
    }
  }

  // MARK: - AppDelegate forwarding

  func savePipelineStages() { pipeline.savePipelineStages() }
  func saveEQPresets() { pipeline.saveEQPresets() }
}
