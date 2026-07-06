import Foundation

public enum ProcessorType: String, Codable, Sendable {
  case compressor = "Compressor"
  case noiseGate = "NoiseGate"
  case race = "RACE"
}

public struct CompressorParameters: Codable, Sendable, Equatable {
  public var channels: Int
  public var monitorChannels: [Int]?
  public var processChannels: [Int]?
  public var attack: Double
  public var release: Double
  public var threshold: Double
  public var factor: Double
  public var makeupGain: Double?
  public var softClip: Bool?
  public var clipLimit: Double?

  enum CodingKeys: String, CodingKey {
    case channels
    case monitorChannels = "monitor_channels"
    case processChannels = "process_channels"
    case attack, release, threshold, factor
    case makeupGain = "makeup_gain"
    case softClip = "soft_clip"
    case clipLimit = "clip_limit"
  }

  public init(
    channels: Int, monitorChannels: [Int]? = nil, processChannels: [Int]? = nil,
    attack: Double, release: Double, threshold: Double, factor: Double,
    makeupGain: Double? = nil, softClip: Bool? = nil, clipLimit: Double? = nil
  ) {
    self.channels = channels
    self.monitorChannels = monitorChannels
    self.processChannels = processChannels
    self.attack = attack
    self.release = release
    self.threshold = threshold
    self.factor = factor
    self.makeupGain = makeupGain
    self.softClip = softClip
    self.clipLimit = clipLimit
  }

  public func monitorChannelsArray() -> [Int] {
    return monitorChannels ?? []
  }

  public func processChannelsArray() -> [Int] {
    return processChannels ?? []
  }

  public func makeupGainValue() -> Double {
    return makeupGain ?? 0.0
  }

  public func softClipValue() -> Bool {
    return softClip ?? false
  }
}

public struct NoiseGateParameters: Codable, Sendable, Equatable {
  public var channels: Int
  public var monitorChannels: [Int]?
  public var processChannels: [Int]?
  public var attack: Double
  public var release: Double
  public var threshold: Double
  public var attenuation: Double

  enum CodingKeys: String, CodingKey {
    case channels
    case monitorChannels = "monitor_channels"
    case processChannels = "process_channels"
    case attack, release, threshold, attenuation
  }

  public init(
    channels: Int, monitorChannels: [Int]? = nil, processChannels: [Int]? = nil,
    attack: Double, release: Double, threshold: Double, attenuation: Double
  ) {
    self.channels = channels
    self.monitorChannels = monitorChannels
    self.processChannels = processChannels
    self.attack = attack
    self.release = release
    self.threshold = threshold
    self.attenuation = attenuation
  }

  public func monitorChannelsArray() -> [Int] {
    return monitorChannels ?? []
  }

  public func processChannelsArray() -> [Int] {
    return processChannels ?? []
  }
}

public struct RACEParameters: Codable, Sendable, Equatable {
  public var channels: Int
  public var channelA: Int
  public var channelB: Int
  public var delay: Double
  public var subsampleDelay: Bool?
  public var delayUnit: DelayUnit?
  public var attenuation: Double

  enum CodingKeys: String, CodingKey {
    case channels
    case channelA = "channel_a"
    case channelB = "channel_b"
    case delay
    case subsampleDelay = "subsample_delay"
    case delayUnit = "delay_unit"
    case attenuation
  }

  public init(
    channels: Int, channelA: Int, channelB: Int, delay: Double,
    subsampleDelay: Bool? = nil, delayUnit: DelayUnit? = nil, attenuation: Double
  ) {
    self.channels = channels
    self.channelA = channelA
    self.channelB = channelB
    self.delay = delay
    self.subsampleDelay = subsampleDelay
    self.delayUnit = delayUnit
    self.attenuation = attenuation
  }

  public func subsampleDelayValue() -> Bool {
    return subsampleDelay ?? false
  }

  public func delayUnitValue() -> DelayUnit {
    return delayUnit ?? .ms
  }
}

public enum ProcessorConfig: Codable, Sendable, Equatable {
  case compressor(CompressorParameters)
  case noiseGate(NoiseGateParameters)
  case race(RACEParameters)

  enum CodingKeys: String, CodingKey {
    case type, parameters
  }

  public var type: ProcessorType {
    switch self {
    case .compressor: return .compressor
    case .noiseGate: return .noiseGate
    case .race: return .race
    }
  }

  public init(from decoder: Decoder) throws {
    let container = try decoder.container(keyedBy: CodingKeys.self)
    let type = try container.decode(ProcessorType.self, forKey: .type)

    switch type {
    case .compressor:
      let p = try container.decode(CompressorParameters.self, forKey: .parameters)
      self = .compressor(p)
    case .noiseGate:
      let p = try container.decode(NoiseGateParameters.self, forKey: .parameters)
      self = .noiseGate(p)
    case .race:
      let p = try container.decode(RACEParameters.self, forKey: .parameters)
      self = .race(p)
    }
  }

  public func encode(to encoder: Encoder) throws {
    var container = encoder.container(keyedBy: CodingKeys.self)
    try container.encode(type, forKey: .type)

    switch self {
    case .compressor(let p):
      try container.encode(p, forKey: .parameters)
    case .noiseGate(let p):
      try container.encode(p, forKey: .parameters)
    case .race(let p):
      try container.encode(p, forKey: .parameters)
    }
  }
}

extension ProcessorConfig {
  public func validate() throws {
    switch self {
    case .compressor(let p):
      guard p.attack > 0 else {
        throw ConfigError.invalidFilter("Compressor: attack must be > 0, got \(p.attack)")
      }
      guard p.release > 0 else {
        throw ConfigError.invalidFilter("Compressor: release must be > 0, got \(p.release)")
      }
      for ch in p.monitorChannelsArray() {
        guard ch < p.channels else {
          throw ConfigError.invalidFilter(
            "Compressor: monitor channel \(ch) is invalid (max: \(p.channels - 1))")
        }
      }
      for ch in p.processChannelsArray() {
        guard ch < p.channels else {
          throw ConfigError.invalidFilter(
            "Compressor: process channel \(ch) is invalid (max: \(p.channels - 1))")
        }
      }
    case .noiseGate(let p):
      guard p.attack > 0 else {
        throw ConfigError.invalidFilter("NoiseGate: attack must be > 0, got \(p.attack)")
      }
      guard p.release > 0 else {
        throw ConfigError.invalidFilter("NoiseGate: release must be > 0, got \(p.release)")
      }
      for ch in p.monitorChannelsArray() {
        guard ch < p.channels else {
          throw ConfigError.invalidFilter(
            "NoiseGate: monitor channel \(ch) is invalid (max: \(p.channels - 1))")
        }
      }
      for ch in p.processChannelsArray() {
        guard ch < p.channels else {
          throw ConfigError.invalidFilter(
            "NoiseGate: process channel \(ch) is invalid (max: \(p.channels - 1))")
        }
      }
    case .race(let p):
      guard p.attenuation > 0 else {
        throw ConfigError.invalidFilter("RACE: attenuation must be > 0, got \(p.attenuation)")
      }
      guard p.delay > 0 else {
        throw ConfigError.invalidFilter("RACE: delay must be > 0, got \(p.delay)")
      }
      guard p.channelA != p.channelB else {
        throw ConfigError.invalidFilter(
          "RACE: channels A and B must be different, got both \(p.channelA)")
      }
      guard p.channelA < p.channels else {
        throw ConfigError.invalidFilter(
          "RACE: channel A \(p.channelA) is invalid (max: \(p.channels - 1))")
      }
      guard p.channelB < p.channels else {
        throw ConfigError.invalidFilter(
          "RACE: channel B \(p.channelB) is invalid (max: \(p.channels - 1))")
      }
    }
  }
}
