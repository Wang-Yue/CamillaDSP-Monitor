import Foundation

public enum AudioBackendError: Error, LocalizedError, Sendable {
  case configParse(message: String)
  case commandSend(message: String)
  case invalidSamplerate(message: String)
  case spectrumCompute(message: String)

  public var errorDescription: String? {
    switch self {
    case .configParse(let message): return "Config parse error: \(message)"
    case .commandSend(let message): return "Command send error: \(message)"
    case .invalidSamplerate(let message): return "Invalid samplerate: \(message)"
    case .spectrumCompute(let message): return "Spectrum compute error: \(message)"
    }
  }

  init(_ dspError: DspError) {
    switch dspError {
    case .ConfigParseError(let message): self = .configParse(message: message)
    case .CommandSendError(let message): self = .commandSend(message: message)
    case .InvalidSamplerate(let message): self = .invalidSamplerate(message: message)
    case .SpectrumComputeError(let message): self = .spectrumCompute(message: message)
    }
  }
}

/// VU level data.
public struct VuLevels: Sendable {
  public let playback_rms: [Float]
  public let playback_peak: [Float]
  public let capture_rms: [Float]
  public let capture_peak: [Float]
}

public enum ProcessingState: Sendable {
  case running
  case paused
  case inactive
  case starting
  case stalled

  init(_ dspState: DspState) {
    switch dspState {
    case .running: self = .running
    case .paused: self = .paused
    case .inactive: self = .inactive
    case .starting: self = .starting
    case .stalled: self = .stalled
    }
  }
}

public enum ProcessingStopReason: Sendable {
  case none
  case done
  case captureError(String)
  case playbackError(String)
  case captureFormatChange(Int)
  case playbackFormatChange(Int)
  case unknownError(String)

  init(_ dspStopReason: DspStopReason) {
    switch dspStopReason {
    case .none: self = .none
    case .done: self = .done
    case .captureError(let message): self = .captureError(message)
    case .playbackError(let message): self = .playbackError(message)
    case .captureFormatChange(let rate): self = .captureFormatChange(Int(rate))
    case .playbackFormatChange(let rate): self = .playbackFormatChange(Int(rate))
    case .unknownError(let message): self = .unknownError(message)
    }
  }
}

/// State change data.
public struct StateUpdate: Sendable {
  public let state: ProcessingState
  public let stopReason: ProcessingStopReason
}

/// Spectrum data.
public struct Spectrum: Sendable {
  public let frequencies: [Float]
  public let magnitudes: [Float]
}

public struct AudioDevice: Identifiable, Sendable {
  public var id: String { name }
  public let name: String
}

// MARK: - Device Capabilities (from GetCaptureDeviceCapabilities / GetPlaybackDeviceCapabilities)

public struct SamplerateCapability: Codable, Sendable, Equatable {
  public let samplerate: Int
  public let formats: [String]
}

public struct ChannelCapability: Codable, Sendable, Equatable {
  public let channels: Int
  public let samplerates: [SamplerateCapability]
}

public struct DeviceCapabilitySet: Codable, Sendable, Equatable {
  public let capabilities: [ChannelCapability]
}

public struct AudioDeviceDescriptor: Codable, Sendable, Equatable {
  public let name: String
  public let capability_sets: [DeviceCapabilitySet]

  public init(
    name: String = "", capability_sets: [DeviceCapabilitySet] = []
  ) {
    self.name = name
    self.capability_sets = capability_sets
  }
}

public actor DSPEngine {
  let engine: CamillaEngine = CamillaEngine()

  public init() {
    print("[DSPEngine] Initializing CamillaDSP library engine...")
  }

  // MARK: - Commands

  public func start(configJson: String) async throws {
    do {
      try engine.setConfig(json: configJson)
    } catch let error as DspError {
      throw AudioBackendError(error)
    }
  }

  public func stop() async {
    engine.stop()
  }

  public func setVolume(_ db: Float) async {
    engine.setVolume(volume: db)
  }

  public func setMute(_ mute: Bool) async {
    engine.setMute(mute: mute)
  }

  public func getAvailableDevices(backend: String, input: Bool) async -> [AudioDevice] {
    let devices = engine.getAvailableDevices(backend: backend, input: input)
    return devices.map { AudioDevice(name: $0) }
  }

  public func getDeviceCapabilities(
    backend: String, device: String, isCapture: Bool
  ) async -> AudioDeviceDescriptor? {
    let json = engine.getDeviceCapabilities(backend: backend, device: device, input: isCapture)
    guard let data = json.data(using: .utf8) else { return nil }
    do {
      return try JSONDecoder().decode(AudioDeviceDescriptor.self, from: data)
    } catch {
      print("[DSPEngine] Failed to decode device capabilities: \(error)")
      return nil
    }
  }

  // MARK: - Direct Fetch APIs

  public func getVuLevels() async -> VuLevels {
    let levels = engine.getVuLevels()
    return VuLevels(
      playback_rms: levels.playbackRms,
      playback_peak: levels.playbackPeak,
      capture_rms: levels.captureRms,
      capture_peak: levels.capturePeak
    )
  }

  public func getStatus() async -> StateUpdate {
    let status = engine.getStatus()
    return StateUpdate(
      state: ProcessingState(status.state),
      stopReason: ProcessingStopReason(status.stopReason)
    )
  }

  public func getSpectrum(
    isCapture: Bool, channel: UInt32?, minFreq: Double, maxFreq: Double, nBins: UInt32
  ) async throws -> Spectrum {
    do {
      let data = try engine.getSpectrum(
        input: isCapture, channel: channel, minFreq: minFreq, maxFreq: maxFreq, nBins: nBins)
      return Spectrum(frequencies: data.frequencies, magnitudes: data.magnitudes)
    } catch let error as DspError {
      throw AudioBackendError(error)
    }
  }

  public func setLogLevel(_ level: String) async {
    engine.setLogLevel(level: level)
  }
}
