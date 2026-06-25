// Standalone Engine Configuration and API Types

import Foundation

/// Engine processing state.
public enum ProcessingState: String, Codable, Sendable, Equatable {
  case inactive = "Inactive"
  case starting = "Starting"
  case running = "Running"
  case paused = "Paused"
  case stalled = "Stalled"

  public var rawByte: UInt8 {
    switch self {
    case .inactive: return 0
    case .starting: return 1
    case .running: return 2
    case .paused: return 3
    case .stalled: return 4
    }
  }

  public init(rawByte: UInt8) {
    switch rawByte {
    case 1: self = .starting
    case 2: self = .running
    case 3: self = .paused
    case 4: self = .stalled
    default: self = .inactive
    }
  }
}

/// Why the engine stopped.
public enum ProcessingStopReason: Sendable, Equatable {
  case none
  case done
  case captureError(String)
  case playbackError(String)
  case captureFormatChange(Int)
  case playbackFormatChange(Int)
  case unknownError(String)
}

public struct StateUpdate: Sendable {
  public let state: ProcessingState
  public let stopReason: ProcessingStopReason

  public init(state: ProcessingState, stopReason: ProcessingStopReason) {
    self.state = state
    self.stopReason = stopReason
  }
}

public struct AudioDevice: Identifiable, Sendable, Equatable {
  public var id: String { name }
  public let name: String
  public init(name: String) { self.name = name }
}

public enum AudioBackendError: Error, LocalizedError, Sendable {
  case configParse(message: String)
  case commandSend(message: String)
  case invalidSamplerate(message: String)
  case spectrumCompute(message: String)
  case engineNotRunning
  case bufferEmpty

  public var errorDescription: String? {
    switch self {
    case .configParse(let m): return "Config parse error: \(m)"
    case .commandSend(let m): return "Command send error: \(m)"
    case .invalidSamplerate(let m): return "Invalid samplerate: \(m)"
    case .spectrumCompute(let m): return "Spectrum compute error: \(m)"
    case .engineNotRunning: return "Engine not running"
    case .bufferEmpty: return "Audio history buffer is empty"
    }
  }
}

// MARK: - Capability data model

public enum SampleFormat: String, Codable, CaseIterable, Sendable {
  case s16 = "S16"
  case s24 = "S24"
  case s32 = "S32"
  case f32 = "F32"
}

public struct SamplerateCapability: Codable, Sendable, Equatable {
  public let samplerate: Int
  public let formats: [String]

  public init(samplerate: Int, formats: [String]) {
    self.samplerate = samplerate
    self.formats = formats
  }
}

public struct ChannelCapability: Codable, Sendable, Equatable {
  public let channels: Int
  public let samplerates: [SamplerateCapability]

  public init(channels: Int, samplerates: [SamplerateCapability]) {
    self.channels = channels
    self.samplerates = samplerates
  }
}

public struct DeviceCapabilitySet: Codable, Sendable, Equatable {
  public let capabilities: [ChannelCapability]

  public init(capabilities: [ChannelCapability]) {
    self.capabilities = capabilities
  }
}

public struct AudioDeviceDescriptor: Codable, Sendable, Equatable {
  public let name: String
  public let capability_sets: [DeviceCapabilitySet]

  public init(name: String = "", capability_sets: [DeviceCapabilitySet] = []) {
    self.name = name
    self.capability_sets = capability_sets
  }
}

// MARK: - Device Config Models

/// Audio I/O backend. DSPMonitor only ever uses CoreAudio.
public enum AudioBackendType: String, Codable, Equatable, Sendable {
  case coreAudio = "CoreAudio"
}

public struct CaptureDeviceConfig: Codable, Equatable, Sendable {
  public var type: AudioBackendType
  public var channels: Int
  public var device: String?

  enum CodingKeys: String, CodingKey {
    case type, channels, device
  }

  public init(
    type: AudioBackendType, channels: Int, device: String? = nil, format: String? = nil
  ) {
    _ = format
    self.type = type
    self.channels = channels
    self.device = device
  }
}

public struct PlaybackDeviceConfig: Codable, Equatable, Sendable {
  public var type: AudioBackendType
  public var channels: Int
  public var device: String?
  public var exclusive: Bool?

  enum CodingKeys: String, CodingKey {
    case type, channels, device, exclusive
  }
  public init(
    type: AudioBackendType, channels: Int, device: String? = nil,
    exclusive: Bool? = nil
  ) {
    self.type = type
    self.channels = channels
    self.device = device
    self.exclusive = exclusive
  }
}

public struct DevicesConfig: Codable, Equatable, Sendable {
  public var samplerate: Int
  public var chunksize: Int
  public var enableRateAdjust: Bool?
  public var targetLevel: Int?
  public var adjustPeriod: Double?
  public var resampler: ResamplerConfig?
  public var capture: CaptureDeviceConfig
  public var playback: PlaybackDeviceConfig
  /// Capture sample rate when different from playback (requires resampler)
  public var captureSamplerate: Int?
  /// Silence detection threshold (dB). 0 = disabled.
  public var silenceThreshold: Double?
  /// Silence detection timeout (seconds). 0 = disabled.
  public var silenceTimeout: Double?

  enum CodingKeys: String, CodingKey {
    case samplerate, chunksize, resampler, capture, playback
    case enableRateAdjust = "enable_rate_adjust"
    case targetLevel = "target_level"
    case adjustPeriod = "adjust_period"
    case captureSamplerate = "capture_samplerate"
    case silenceThreshold = "silence_threshold"
    case silenceTimeout = "silence_timeout"
  }

  public init(
    samplerate: Int, chunksize: Int, capture: CaptureDeviceConfig, playback: PlaybackDeviceConfig
  ) {
    self.samplerate = samplerate
    self.chunksize = chunksize
    self.capture = capture
    self.playback = playback
  }
}
