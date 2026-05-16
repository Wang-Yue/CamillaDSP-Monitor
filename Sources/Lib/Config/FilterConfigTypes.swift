// Standalone filter configuration types.

import Foundation

public enum FilterType: String, Codable, Sendable {
  case gain = "Gain"
  case volume = "Volume"
  case loudness = "Loudness"
  case biquad = "Biquad"
  case conv = "Conv"
}

public enum GainScale: String, Codable, Sendable {
  case dB
  case linear
}

public struct GainParameters: Codable, Sendable {
  public var gain: Double?
  public var scale: GainScale?
  public var inverted: Bool?
  public var mute: Bool?

  enum CodingKeys: String, CodingKey {
    case gain, scale, inverted, mute
  }

  public init(
    name: String? = nil, gain: Double? = nil, scale: GainScale? = nil, inverted: Bool? = nil,
    mute: Bool? = nil
  ) {
    _ = name
    self.gain = gain
    self.scale = scale
    self.inverted = inverted
    self.mute = mute
  }

  public func validate() throws {
    if let gain = gain {
      guard gain > -150 && gain < 150 else {
        throw ConfigError.invalidFilter("gain must be in (-150, 150) dB, got \(gain)")
      }
    }
  }
}

public struct LoudnessParameters: Codable, Sendable {
  public var referenceLevel: Double?
  public var highBoost: Double?
  public var lowBoost: Double?
  public var attenuateMid: Bool?

  enum CodingKeys: String, CodingKey {
    case referenceLevel = "reference_level"
    case highBoost = "high_boost"
    case lowBoost = "low_boost"
    case attenuateMid = "attenuate_mid"
  }

  public init(
    referenceLevel: Double? = nil, highBoost: Double? = nil,
    lowBoost: Double? = nil,
    attenuateMid: Bool? = nil
  ) {
    self.referenceLevel = referenceLevel
    self.highBoost = highBoost
    self.lowBoost = lowBoost
    self.attenuateMid = attenuateMid
  }

  public func validate() throws {
    if let ref = referenceLevel {
      guard ref > -100 && ref < 20 else {
        throw ConfigError.invalidFilter("reference_level must be in (-100, 20), got \(ref)")
      }
    }
    if let boost = highBoost {
      guard boost >= 0 && boost <= 20 else {
        throw ConfigError.invalidFilter("high_boost must be in [0, 20], got \(boost)")
      }
    }
    if let boost = lowBoost {
      guard boost >= 0 && boost <= 20 else {
        throw ConfigError.invalidFilter("low_boost must be in [0, 20], got \(boost)")
      }
    }
  }
}

public enum BiquadType: String, Codable, Sendable {
  case highpass = "Highpass"
  case lowpass = "Lowpass"
  case highpassFO = "HighpassFO"
  case lowpassFO = "LowpassFO"
  case highshelf = "Highshelf"
  case lowshelf = "Lowshelf"
  case highshelfFO = "HighshelfFO"
  case lowshelfFO = "LowshelfFO"
  case peaking = "Peaking"
  case notch = "Notch"
  case bandpass = "Bandpass"
  case allpass = "Allpass"
  case allpassFO = "AllpassFO"
}

public struct BiquadParameters: Codable, Sendable {
  public var type: BiquadType?
  public var freq: Double?
  public var gain: Double?
  public var q: Double?
  public var bandwidth: Double?
  public var slope: Double?

  enum CodingKeys: String, CodingKey {
    case type, freq, gain, q, bandwidth, slope
  }

  public init(
    name: String? = nil, type: BiquadType? = nil, freq: Double? = nil, gain: Double? = nil,
    q: Double? = nil,
    bandwidth: Double? = nil, slope: Double? = nil
  ) {
    _ = name
    self.type = type
    self.freq = freq
    self.gain = gain
    self.q = q
    self.bandwidth = bandwidth
    self.slope = slope
  }
}

public enum ConvType: String, Codable, Sendable {
  case values = "Values"
  case wav = "Wav"
  case raw = "Raw"
  case dummy = "Dummy"
}

public struct ConvParameters: Codable, Sendable {
  public var type: ConvType
  public var values: [Double]?
  public var filename: String?
  public var format: String?
  public var channel: Int?
  public var length: Int?

  enum CodingKeys: String, CodingKey {
    case type, values, filename, format, channel, length
  }

  public init(
    type: ConvType,
    values: [Double]? = nil,
    filename: String? = nil,
    format: String? = nil,
    channel: Int? = nil,
    length: Int? = nil
  ) {
    self.type = type
    self.values = values
    self.filename = filename
    self.format = format
    self.channel = channel
    self.length = length
  }

  public func validate() throws {
    switch type {
    case .values:
      guard let v = values, !v.isEmpty else {
        throw ConfigError.invalidFilter("Conv 'values' must be non-empty")
      }
    case .wav, .raw:
      guard let f = filename, !f.isEmpty else {
        throw ConfigError.invalidFilter("Conv '\(type.rawValue)' missing filename")
      }
    case .dummy:
      guard let n = length, n > 0 else {
        throw ConfigError.invalidFilter("Conv 'dummy' length must be > 0")
      }
    }
  }
}

public enum FilterConfig: Codable, Sendable {
  case gain(GainParameters)
  case volume
  case loudness(LoudnessParameters)
  case biquad(BiquadParameters)
  case conv(ConvParameters)

  public var type: FilterType {
    switch self {
    case .gain: return .gain
    case .volume: return .volume
    case .loudness: return .loudness
    case .biquad: return .biquad
    case .conv: return .conv
    }
  }

  public func validate() throws {
    switch self {
    case .biquad:
      // Validation logic for biquad parameters can run via extensions in DSPFilters where BiquadCoefficients live
      break
    case .gain(let params):
      try params.validate()
    case .loudness(let params):
      try params.validate()
    case .conv(let params):
      try params.validate()
    case .volume:
      break
    }
  }

  enum CodingKeys: String, CodingKey {
    case type, parameters
  }

  public init(from decoder: Decoder) throws {
    let container = try decoder.container(keyedBy: CodingKeys.self)
    let type = try container.decode(FilterType.self, forKey: .type)

    switch type {
    case .gain:
      let p = try container.decode(GainParameters.self, forKey: .parameters)
      self = .gain(p)
    case .volume:
      self = .volume
    case .loudness:
      let p = try container.decode(LoudnessParameters.self, forKey: .parameters)
      self = .loudness(p)
    case .biquad:
      let p = try container.decode(BiquadParameters.self, forKey: .parameters)
      self = .biquad(p)
    case .conv:
      let p = try container.decode(ConvParameters.self, forKey: .parameters)
      self = .conv(p)
    }
  }

  public func encode(to encoder: Encoder) throws {
    var container = encoder.container(keyedBy: CodingKeys.self)
    try container.encode(type, forKey: .type)

    switch self {
    case .gain(let p):
      try container.encode(p, forKey: .parameters)
    case .volume:
      break
    case .loudness(let p):
      try container.encode(p, forKey: .parameters)
    case .biquad(let p):
      try container.encode(p, forKey: .parameters)
    case .conv(let p):
      try container.encode(p, forKey: .parameters)
    }
  }
}
