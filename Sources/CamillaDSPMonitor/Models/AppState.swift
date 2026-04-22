// AppState - Thin coordinator that owns and wires all domain objects

import CamillaDSPLib
import Foundation
import SwiftUI

public enum ResamplerType: String, Codable, Sendable, CaseIterable, Identifiable {
  case asyncSinc = "AsyncSinc"
  case asyncPoly = "AsyncPoly"
  case synchronous = "Synchronous"
  public var id: String { rawValue }
}

public enum ResamplerProfile: String, Codable, Sendable, CaseIterable, Identifiable {
  case veryFast = "VeryFast"
  case fast = "Fast"
  case balanced = "Balanced"
  case accurate = "Accurate"
  public var id: String { rawValue }
}

public enum ResamplerInterpolation: String, Codable, Sendable, CaseIterable, Identifiable {
  case linear = "Linear"
  case quadratic = "Quadratic"
  case cubic = "Cubic"
  case sinc = "Sinc"
  public var id: String { rawValue }
}

public enum AppStatus: Equatable, Sendable {
  case inactive
  case starting
  case running
  case paused
  case stalled
}

@MainActor
final class AppState: ObservableObject {
  @Published var isMiniPlayerActive = false

  let engine: DSPEngine
  let settings: AudioSettings
  let pipeline: PipelineStore
  let devices: AudioDeviceManager
  let monitoring: MonitoringController
  let dsp: DSPEngineController
  let spectrum: SpectrumEngine
  let levels: LevelState
  let load: LoadState
  let logManager = LogManager()

  init() {
    print("[AppState] Initializing...")
    DSPEngine.killStaleCamillaDSP()

    let engine = DSPEngine()
    let settings = AudioSettings()
    let pipeline = PipelineStore()
    let levels = LevelState()
    let load = LoadState()
    let devices = AudioDeviceManager(engine: engine, settings: settings)
    let monitoring = MonitoringController(
      engine: engine, levels: levels,
      devices: devices, settings: settings)
    let dsp = DSPEngineController(
      engine: engine, devices: devices, settings: settings, pipeline: pipeline,
      monitoring: monitoring, levels: levels, load: load)
    let spectrum = SpectrumEngine(dsp: dsp, devices: devices, settings: settings)

    self.engine = engine
    self.settings = settings
    self.pipeline = pipeline
    self.devices = devices
    self.monitoring = monitoring
    self.dsp = dsp
    self.spectrum = spectrum
    // Assign the same instances that monitoring/dsp received — NOT new default-value instances.
    // Without these assignments the stored-property slots would hold different LevelState /
    // LoadState objects than the sub-controllers update, so views would never observe changes.
    self.levels = levels
    self.load = load

    // Wire callbacks after all objects exist. onChanged is nil during loadPreferences()
    // and the initial captureConfig/playbackConfig assignments, so no premature applyConfig
    // fires during startup (the status == .running guard in applyConfig() provides a
    // secondary backstop).
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

    // Load persisted preferences (callbacks are now wired but engine isn't running yet).
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
