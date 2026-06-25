import DSPAudio
import DSPBackend
import DSPConfig
import DSPLogging
import DSPPipeline
import Foundation
import Synchronization

public actor SwiftDSPEngine {
  private let logger = Logger(label: "dsp.engine")
  private var core: DSPEngineCore?
  private var desiredVolumeDb: PrcFmt = 0.0
  private var desiredMute: Bool = false
  private var lastStopReason: ProcessingStopReason?

  public init() {}

  public func setConfig(json: String) async throws {
    logger.info("Set config: %s", .string(json))
    let parsed: DSPConfiguration
    do {
      parsed = try ConfigLoader.parse(json: json)
    } catch {
      if let existing = core {
        existing.stop(reason: .none)
        core = nil
      }
      throw AudioBackendError.configParse(message: "\(error)")
    }

    if let existing = core,
      existing.state != .inactive,
      existing.currentConfig.devices == parsed.devices
    {
      do {
        try existing.reloadConfig(parsed)
      } catch {
        existing.stop(reason: .none)
        core = nil
        throw AudioBackendError.configParse(message: "\(error)")
      }
      return
    }

    if let previous = core, previous.state != .inactive {
      previous.stop(reason: .none)
    }

    if let previous = core {
      var waited: TimeInterval = 0
      while previous.state != .inactive, waited < 1.0 {
        try? await Task.sleep(nanoseconds: 5_000_000)
        waited += 0.005
      }
    }
    core = nil

    let engine = DSPEngineCore(config: parsed)
    engine.processingParams.targetVolume = desiredVolumeDb
    engine.processingParams.currentVolume = desiredVolumeDb
    engine.processingParams.isMuted = desiredMute

    do {
      try engine.start()
    } catch {
      throw AudioBackendError.commandSend(message: "\(error)")
    }
    self.core = engine
    self.lastStopReason = nil
  }

  public func stop() {
    if let engine = core, engine.state != .inactive {
      engine.stop(reason: .none)
      lastStopReason = ProcessingStopReason.none
    }
    core = nil
  }

  public func setVolume(_ db: Float) {
    desiredVolumeDb = PrcFmt(db)
    core?.processingParams.targetVolume = PrcFmt(db)
  }

  public func setMute(_ mute: Bool) {
    desiredMute = mute
    core?.processingParams.isMuted = mute
  }

  public func getStatus() -> StateUpdate {
    let state: ProcessingState
    let reason: ProcessingStopReason
    if let core {
      state = core.state
      reason = core.stopReason ?? lastStopReason ?? .none
    } else {
      state = .inactive
      reason = lastStopReason ?? .none
    }
    return StateUpdate(state: state, stopReason: reason)
  }

  public func setLogLevel(_ level: LogLevel) {
    MutableLogLevel.current = level
  }
  public func getAvailableDevices(backend: String, input: Bool) -> [AudioDevice] {
    guard backend.lowercased() == "coreaudio" else { return [] }
    let raw = input ? CoreAudioCapture.listDevices() : CoreAudioPlayback.listDevices()
    return raw.map { AudioDevice(name: $0.name) }
  }

  public func getDeviceCapabilities(backend: String, device: String, isCapture: Bool)
    -> AudioDeviceDescriptor?
  {
    guard backend.lowercased() == "coreaudio" else { return nil }
    return CoreAudioCapabilities.describe(deviceName: device, isCapture: isCapture)
  }
}
