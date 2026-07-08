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

public struct VuLevels: Codable, Sendable {
  public let playback_rms: [Float]
  public let playback_peak: [Float]
  public let capture_rms: [Float]
  public let capture_peak: [Float]

  public init(
    playback_rms: [Float], playback_peak: [Float], capture_rms: [Float], capture_peak: [Float]
  ) {
    self.playback_rms = playback_rms
    self.playback_peak = playback_peak
    self.capture_rms = capture_rms
    self.capture_peak = capture_peak
  }
}

public struct Spectrum: Codable, Sendable {
  public let frequencies: [Float]
  public let magnitudes: [Float]

  public init(frequencies: [Float], magnitudes: [Float]) {
    self.frequencies = frequencies
    self.magnitudes = magnitudes
  }
}

public struct AudioSamples: Codable, Sendable {
  public let channels: [[Float]]

  public init(channels: [[Float]]) {
    self.channels = channels
  }

  public var left: [Float] { channels.first ?? [] }
  public var right: [Float] { channels.count > 1 ? channels[1] : (channels.first ?? []) }
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

public enum AudioBackendType: String, Codable, Equatable, Sendable, CaseIterable {
  case coreAudio = "CoreAudio"
  case rawFile = "RawFile"
  case wavFile = "WavFile"
  case signalGenerator = "SignalGenerator"
}

public struct GeneratorConfig: Codable, Equatable, Sendable {
  public var type: String
  public var freq: Double?
  public var level: Double

  public init(type: String = "Sine", freq: Double? = 1000.0, level: Double = -6.0) {
    self.type = type
    self.freq = freq
    self.level = level
  }
}

public struct CoreAudioCaptureConfig: Codable, Equatable, Sendable {
  public var channels: Int
  public var device: String?
  public var format: String?
  public var bypassDoP: Bool?
  public var dopCutoffHz: Double?
  public var channelLabels: [String]?

  enum CodingKeys: String, CodingKey {
    case channels, device
    case format
    case bypassDoP = "bypass_dop"
    case dopCutoffHz = "dop_cutoff_hz"
    case channelLabels = "channel_labels"
  }

  public init(
    channels: Int,
    device: String? = nil,
    format: String? = nil,
    bypassDoP: Bool? = nil,
    dopCutoffHz: Double? = nil,
    channelLabels: [String]? = nil
  ) {
    self.channels = channels
    self.device = device
    self.format = format
    self.bypassDoP = bypassDoP
    self.dopCutoffHz = dopCutoffHz
    self.channelLabels = channelLabels
  }
}

public enum SDMFilter: String, Codable, CaseIterable, Sendable, ExpressibleByStringLiteral {
  case clans4 = "clans-4"
  case sdm4 = "sdm-4"
  case clans5 = "clans-5"
  case sdm5 = "sdm-5"
  case clans6 = "clans-6"
  case sdm6 = "sdm-6"
  case clans7 = "clans-7"
  case sdm7 = "sdm-7"
  case clans8 = "clans-8"
  case sdm8 = "sdm-8"

  public init(stringLiteral value: String) {
    if let val = SDMFilter(rawValue: value) {
      self = val
    } else {
      fatalError("Invalid SDMFilter: \(value)")
    }
  }
}

public struct CoreAudioPlaybackConfig: Codable, Equatable, Sendable {
  public var channels: Int
  public var device: String?
  public var format: String?
  public var exclusive: Bool?
  public var outputDoP: Bool?
  public var dopEncoderFilter: SDMFilter?
  public var channelLabels: [String]?

  enum CodingKeys: String, CodingKey {
    case channels, device
    case format
    case exclusive
    case outputDoP = "output_dop"
    case dopEncoderFilter = "dop_encoder_filter"
    case channelLabels = "channel_labels"
  }

  public init(
    channels: Int,
    device: String? = nil,
    format: String? = nil,
    exclusive: Bool? = nil,
    outputDoP: Bool? = nil,
    dopEncoderFilter: SDMFilter? = nil,
    channelLabels: [String]? = nil
  ) {
    self.channels = channels
    self.device = device
    self.format = format
    self.exclusive = exclusive
    self.outputDoP = outputDoP
    self.dopEncoderFilter = dopEncoderFilter
    self.channelLabels = channelLabels
  }
}

public struct WavFileCaptureConfig: Codable, Equatable, Sendable {
  public var filename: String
  public var extraSamples: Int?
  public var channelLabels: [String]?

  enum CodingKeys: String, CodingKey {
    case filename
    case extraSamples = "extra_samples"
    case channelLabels = "channel_labels"
  }

  public init(
    filename: String,
    extraSamples: Int? = nil,
    channelLabels: [String]? = nil
  ) {
    self.filename = filename
    self.extraSamples = extraSamples
    self.channelLabels = channelLabels
  }
}

public struct RawFileCaptureConfig: Codable, Equatable, Sendable {
  public var channels: Int
  public var filename: String
  public var format: String
  public var skipBytes: Int?
  public var readBytes: Int?
  public var extraSamples: Int?
  public var channelLabels: [String]?

  enum CodingKeys: String, CodingKey {
    case channels, filename, format
    case skipBytes = "skip_bytes"
    case readBytes = "read_bytes"
    case extraSamples = "extra_samples"
    case channelLabels = "channel_labels"
  }

  public init(
    channels: Int,
    filename: String,
    format: String,
    skipBytes: Int? = nil,
    readBytes: Int? = nil,
    extraSamples: Int? = nil,
    channelLabels: [String]? = nil
  ) {
    self.channels = channels
    self.filename = filename
    self.format = format
    self.skipBytes = skipBytes
    self.readBytes = readBytes
    self.extraSamples = extraSamples
    self.channelLabels = channelLabels
  }
}

public struct RawFilePlaybackConfig: Codable, Equatable, Sendable {
  public var channels: Int
  public var filename: String
  public var format: String
  public var wavHeader: Bool?
  public var channelLabels: [String]?

  enum CodingKeys: String, CodingKey {
    case channels, filename, format
    case wavHeader = "wav_header"
    case channelLabels = "channel_labels"
  }

  public init(
    channels: Int,
    filename: String,
    format: String,
    wavHeader: Bool? = nil,
    channelLabels: [String]? = nil
  ) {
    self.channels = channels
    self.filename = filename
    self.format = format
    self.wavHeader = wavHeader
    self.channelLabels = channelLabels
  }
}

public struct GeneratorCaptureConfig: Codable, Equatable, Sendable {
  public var channels: Int
  public var signal: GeneratorConfig
  public var channelLabels: [String]?

  enum CodingKeys: String, CodingKey {
    case channels, signal
    case channelLabels = "channel_labels"
  }

  public init(
    channels: Int,
    signal: GeneratorConfig,
    channelLabels: [String]? = nil
  ) {
    self.channels = channels
    self.signal = signal
    self.channelLabels = channelLabels
  }
}

public enum CaptureDeviceConfig: Equatable, Sendable {
  case coreAudio(CoreAudioCaptureConfig)
  case wavFile(WavFileCaptureConfig)
  case rawFile(RawFileCaptureConfig)
  case signalGenerator(GeneratorCaptureConfig)
}

public enum PlaybackDeviceConfig: Equatable, Sendable {
  case coreAudio(CoreAudioPlaybackConfig)
  case rawFile(RawFilePlaybackConfig)
}

extension CaptureDeviceConfig {
  public var type: AudioBackendType {
    switch self {
    case .coreAudio: return .coreAudio
    case .wavFile: return .wavFile
    case .rawFile: return .rawFile
    case .signalGenerator: return .signalGenerator
    }
  }

  public var channels: Int? {
    switch self {
    case .coreAudio(let cfg): return cfg.channels
    case .rawFile(let cfg): return cfg.channels
    case .signalGenerator(let cfg): return cfg.channels
    case .wavFile: return nil
    }
  }

  public var deviceName: String? {
    switch self {
    case .coreAudio(let cfg): return cfg.device
    default: return nil
    }
  }

  public var device: String? {
    return deviceName
  }

  public var channelLabels: [String]? {
    switch self {
    case .coreAudio(let cfg): return cfg.channelLabels
    case .wavFile(let cfg): return cfg.channelLabels
    case .rawFile(let cfg): return cfg.channelLabels
    case .signalGenerator(let cfg): return cfg.channelLabels
    }
  }

  public mutating func setChannels(_ count: Int) {
    switch self {
    case .coreAudio(var cfg):
      cfg.channels = count
      self = .coreAudio(cfg)
    case .rawFile(var cfg):
      cfg.channels = count
      self = .rawFile(cfg)
    case .signalGenerator(var cfg):
      cfg.channels = count
      self = .signalGenerator(cfg)
    case .wavFile:
      break
    }
  }

  public init(type: AudioBackendType, channels: Int) {
    switch type {
    case .coreAudio:
      self = .coreAudio(CoreAudioCaptureConfig(channels: channels))
    case .rawFile:
      self = .rawFile(RawFileCaptureConfig(channels: channels, filename: "", format: "S16_LE"))
    case .wavFile:
      self = .wavFile(WavFileCaptureConfig(filename: ""))
    case .signalGenerator:
      self = .signalGenerator(GeneratorCaptureConfig(channels: channels, signal: GeneratorConfig()))
    }
  }

  public var bypassDoP: Bool? {
    switch self {
    case .coreAudio(let cfg): return cfg.bypassDoP
    default: return nil
    }
  }

  public var dopCutoffHz: Double? {
    switch self {
    case .coreAudio(let cfg): return cfg.dopCutoffHz
    default: return nil
    }
  }
}

extension PlaybackDeviceConfig {
  public var type: AudioBackendType {
    switch self {
    case .coreAudio: return .coreAudio
    case .rawFile: return .rawFile
    }
  }

  public var channels: Int {
    switch self {
    case .coreAudio(let cfg): return cfg.channels
    case .rawFile(let cfg): return cfg.channels
    }
  }

  public var deviceName: String? {
    switch self {
    case .coreAudio(let cfg): return cfg.device
    default: return nil
    }
  }

  public var device: String? {
    return deviceName
  }

  public var exclusive: Bool? {
    switch self {
    case .coreAudio(let cfg): return cfg.exclusive
    case .rawFile: return nil
    }
  }

  public var channelLabels: [String]? {
    switch self {
    case .coreAudio(let cfg): return cfg.channelLabels
    case .rawFile(let cfg): return cfg.channelLabels
    }
  }

  public var outputDoP: Bool? {
    switch self {
    case .coreAudio(let cfg): return cfg.outputDoP
    default: return nil
    }
  }

  public var dopEncoderFilter: SDMFilter? {
    switch self {
    case .coreAudio(let cfg): return cfg.dopEncoderFilter
    default: return nil
    }
  }

  public init(type: AudioBackendType, channels: Int) {
    switch type {
    case .coreAudio:
      self = .coreAudio(CoreAudioPlaybackConfig(channels: channels))
    case .rawFile:
      self = .rawFile(RawFilePlaybackConfig(channels: channels, filename: "", format: "S16_LE"))
    case .wavFile, .signalGenerator:
      fatalError("Unsupported playback backend: \(type)")
    }
  }
}

extension CaptureDeviceConfig: Codable {
  private enum CodingKeys: String, CodingKey {
    case type
  }

  public init(from decoder: Decoder) throws {
    let container = try decoder.container(keyedBy: CodingKeys.self)
    let typeStr = try container.decode(String.self, forKey: .type)
    guard let type = AudioBackendType(rawValue: typeStr) else {
      throw DecodingError.dataCorruptedError(
        forKey: .type, in: container, debugDescription: "Unknown capture backend type: \(typeStr)")
    }
    switch type {
    case .coreAudio:
      self = .coreAudio(try CoreAudioCaptureConfig(from: decoder))
    case .wavFile:
      self = .wavFile(try WavFileCaptureConfig(from: decoder))
    case .rawFile:
      self = .rawFile(try RawFileCaptureConfig(from: decoder))
    case .signalGenerator:
      self = .signalGenerator(try GeneratorCaptureConfig(from: decoder))
    }
  }

  public func encode(to encoder: Encoder) throws {
    var container = encoder.container(keyedBy: CodingKeys.self)
    switch self {
    case .coreAudio(let cfg):
      try container.encode(AudioBackendType.coreAudio.rawValue, forKey: .type)
      try cfg.encode(to: encoder)
    case .wavFile(let cfg):
      try container.encode(AudioBackendType.wavFile.rawValue, forKey: .type)
      try cfg.encode(to: encoder)
    case .rawFile(let cfg):
      try container.encode(AudioBackendType.rawFile.rawValue, forKey: .type)
      try cfg.encode(to: encoder)
    case .signalGenerator(let cfg):
      try container.encode(AudioBackendType.signalGenerator.rawValue, forKey: .type)
      try cfg.encode(to: encoder)
    }
  }
}

extension PlaybackDeviceConfig: Codable {
  private enum CodingKeys: String, CodingKey {
    case type
  }

  public init(from decoder: Decoder) throws {
    let container = try decoder.container(keyedBy: CodingKeys.self)
    let typeStr = try container.decode(String.self, forKey: .type)
    if typeStr == "File" {
      self = .rawFile(try RawFilePlaybackConfig(from: decoder))
    } else if typeStr == "CoreAudio" {
      self = .coreAudio(try CoreAudioPlaybackConfig(from: decoder))
    } else {
      throw DecodingError.dataCorruptedError(
        forKey: .type, in: container,
        debugDescription: "Unsupported playback backend type: \(typeStr)")
    }
  }

  public func encode(to encoder: Encoder) throws {
    var container = encoder.container(keyedBy: CodingKeys.self)
    switch self {
    case .coreAudio(let cfg):
      try container.encode(AudioBackendType.coreAudio.rawValue, forKey: .type)
      try cfg.encode(to: encoder)
    case .rawFile(let cfg):
      try container.encode("File", forKey: .type)
      try cfg.encode(to: encoder)
    }
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
  public var volumeRampTime: Double?
  public var volumeLimit: Double?

  public var queuelimit: Int?
  public var stopOnRateChange: Bool?
  public var rateMeasureInterval: Double?
  public var multithreaded: Bool?
  public var workerThreads: Int?

  enum CodingKeys: String, CodingKey {
    case samplerate, chunksize, resampler, capture, playback
    case enableRateAdjust = "enable_rate_adjust"
    case targetLevel = "target_level"
    case adjustPeriod = "adjust_period"
    case captureSamplerate = "capture_samplerate"
    case silenceThreshold = "silence_threshold"
    case silenceTimeout = "silence_timeout"
    case volumeRampTime = "volume_ramp_time"
    case volumeLimit = "volume_limit"
    case queuelimit
    case stopOnRateChange = "stop_on_rate_change"
    case rateMeasureInterval = "rate_measure_interval"
    case multithreaded
    case workerThreads = "worker_threads"
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
