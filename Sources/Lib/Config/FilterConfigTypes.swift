// Standalone filter configuration types.

import DSPAudio
import Foundation

public enum FilterType: String, Codable, Sendable {
  case gain = "Gain"
  case volume = "Volume"
  case loudness = "Loudness"
  case biquad = "Biquad"
  case conv = "Conv"
  case delay = "Delay"
  case biquadCombo = "BiquadCombo"
  case diffEq = "DiffEq"
  case dither = "Dither"
  case limiter = "Limiter"
  case lookaheadLimiter = "LookaheadLimiter"
}

public enum GainScale: String, Codable, Sendable {
  case dB
  case linear
}

public struct GainParameters: Codable, Sendable, Equatable {
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

public struct LoudnessParameters: Codable, Sendable, Equatable {
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
  case free = "Free"
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
  case generalNotch = "GeneralNotch"
  case linkwitzTransform = "LinkwitzTransform"
}

public struct BiquadParameters: Codable, Sendable, Equatable {
  public var type: BiquadType?
  public var freq: Double?
  public var gain: Double?
  public var q: Double?
  public var bandwidth: Double?
  public var slope: Double?

  // Free biquad coefficients
  public var a1: Double?
  public var a2: Double?
  public var b0: Double?
  public var b1: Double?
  public var b2: Double?

  // GeneralNotch parameters
  public var freqNotch: Double?
  public var freqPole: Double?
  public var normalizeAtDc: Bool?

  // LinkwitzTransform parameters
  public var freqAct: Double?
  public var qAct: Double?
  public var freqTarget: Double?
  public var qTarget: Double?

  enum CodingKeys: String, CodingKey {
    case type, freq, gain, q, bandwidth, slope
    case a1, a2, b0, b1, b2
    case freqNotch = "freq_notch"
    case freqPole = "freq_pole"
    case normalizeAtDc = "normalize_at_dc"
    case freqAct = "freq_act"
    case qAct = "q_act"
    case freqTarget = "freq_target"
    case qTarget = "q_target"
  }

  public init(
    name: String? = nil, type: BiquadType? = nil, freq: Double? = nil, gain: Double? = nil,
    q: Double? = nil,
    bandwidth: Double? = nil, slope: Double? = nil,
    a1: Double? = nil, a2: Double? = nil, b0: Double? = nil, b1: Double? = nil, b2: Double? = nil,
    freqNotch: Double? = nil, freqPole: Double? = nil, normalizeAtDc: Bool? = nil,
    freqAct: Double? = nil, qAct: Double? = nil, freqTarget: Double? = nil, qTarget: Double? = nil
  ) {
    _ = name
    self.type = type
    self.freq = freq
    self.gain = gain
    self.q = q
    self.bandwidth = bandwidth
    self.slope = slope
    self.a1 = a1
    self.a2 = a2
    self.b0 = b0
    self.b1 = b1
    self.b2 = b2
    self.freqNotch = freqNotch
    self.freqPole = freqPole
    self.normalizeAtDc = normalizeAtDc
    self.freqAct = freqAct
    self.qAct = qAct
    self.freqTarget = freqTarget
    self.qTarget = qTarget
  }
}

public enum ConvType: String, Codable, Sendable {
  case values = "Values"
  case wav = "Wav"
  case raw = "Raw"
  case dummy = "Dummy"
}

public struct ConvParameters: Codable, Sendable, Equatable {
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

public enum FilterConfig: Codable, Sendable, Equatable {
  case gain(GainParameters)
  case volume(VolumeParameters)
  case loudness(LoudnessParameters)
  case biquad(BiquadParameters)
  case conv(ConvParameters)
  case delay(DelayParameters)
  case biquadCombo(BiquadComboParameters)
  case diffEq(DiffEqParameters)
  case dither(DitherParameters)
  case limiter(LimiterParameters)
  case lookaheadLimiter(LookaheadLimiterParameters)

  public var type: FilterType {
    switch self {
    case .gain: return .gain
    case .volume: return .volume
    case .loudness: return .loudness
    case .biquad: return .biquad
    case .conv: return .conv
    case .delay: return .delay
    case .biquadCombo: return .biquadCombo
    case .diffEq: return .diffEq
    case .dither: return .dither
    case .limiter: return .limiter
    case .lookaheadLimiter: return .lookaheadLimiter
    }
  }

  public func validate() throws {
    switch self {
    case .biquad:
      break
    case .gain(let params):
      try params.validate()
    case .loudness(let params):
      try params.validate()
    case .conv(let params):
      try params.validate()
    case .volume(let params):
      try params.validate()
    case .delay(let params):
      try params.validate()
    case .biquadCombo:
      break
    case .diffEq(let params):
      try params.validate()
    case .dither(let params):
      try params.validate()
    case .limiter(let params):
      try params.validate()
    case .lookaheadLimiter:
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
      if let p = try? container.decode(VolumeParameters.self, forKey: .parameters) {
        self = .volume(p)
      } else {
        self = .volume(VolumeParameters())
      }
    case .loudness:
      let p = try container.decode(LoudnessParameters.self, forKey: .parameters)
      self = .loudness(p)
    case .biquad:
      let p = try container.decode(BiquadParameters.self, forKey: .parameters)
      self = .biquad(p)
    case .conv:
      let p = try container.decode(ConvParameters.self, forKey: .parameters)
      self = .conv(p)
    case .delay:
      let p = try container.decode(DelayParameters.self, forKey: .parameters)
      self = .delay(p)
    case .biquadCombo:
      let p = try container.decode(BiquadComboParameters.self, forKey: .parameters)
      self = .biquadCombo(p)
    case .diffEq:
      let p = try container.decode(DiffEqParameters.self, forKey: .parameters)
      self = .diffEq(p)
    case .dither:
      let p = try container.decode(DitherParameters.self, forKey: .parameters)
      self = .dither(p)
    case .limiter:
      let p = try container.decode(LimiterParameters.self, forKey: .parameters)
      self = .limiter(p)
    case .lookaheadLimiter:
      let p = try container.decode(LookaheadLimiterParameters.self, forKey: .parameters)
      self = .lookaheadLimiter(p)
    }
  }

  public func encode(to encoder: Encoder) throws {
    var container = encoder.container(keyedBy: CodingKeys.self)
    try container.encode(type, forKey: .type)

    switch self {
    case .gain(let p):
      try container.encode(p, forKey: .parameters)
    case .volume(let p):
      try container.encode(p, forKey: .parameters)
    case .loudness(let p):
      try container.encode(p, forKey: .parameters)
    case .biquad(let p):
      try container.encode(p, forKey: .parameters)
    case .conv(let p):
      try container.encode(p, forKey: .parameters)
    case .delay(let p):
      try container.encode(p, forKey: .parameters)
    case .biquadCombo(let p):
      try container.encode(p, forKey: .parameters)
    case .diffEq(let p):
      try container.encode(p, forKey: .parameters)
    case .dither(let p):
      try container.encode(p, forKey: .parameters)
    case .limiter(let p):
      try container.encode(p, forKey: .parameters)
    case .lookaheadLimiter(let p):
      try container.encode(p, forKey: .parameters)
    }
  }
}

public enum DelayUnit: String, Codable, Sendable {
  case ms
  case us
  case samples
  case mm
}

public struct DelayParameters: Codable, Sendable, Equatable {
  public var delay: Double
  public var unit: DelayUnit?
  public var subsample: Bool?

  enum CodingKeys: String, CodingKey {
    case delay, unit, subsample
  }

  public init(delay: Double, unit: DelayUnit? = nil, subsample: Bool? = nil) {
    self.delay = delay
    self.unit = unit
    self.subsample = subsample
  }

  public func validate() throws {
    guard delay >= 0 else {
      throw ConfigError.invalidFilter("Delay cannot be negative, got \(delay)")
    }
  }
}

public enum BiquadComboType: String, Codable, Sendable {
  case butterworthHighpass = "ButterworthHighpass"
  case butterworthLowpass = "ButterworthLowpass"
  case linkwitzRileyHighpass = "LinkwitzRileyHighpass"
  case linkwitzRileyLowpass = "LinkwitzRileyLowpass"
  case tilt = "Tilt"
  case fivePointPeq = "FivePointPeq"
  case graphicEqualizer = "GraphicEqualizer"
}

public struct BiquadComboParameters: Codable, Sendable, Equatable {
  public var type: BiquadComboType
  public var freq: Double?
  public var order: Int?
  public var gain: Double?
  public var fls: Double?
  public var qls: Double?
  public var gls: Double?
  public var fp1: Double?
  public var qp1: Double?
  public var gp1: Double?
  public var fp2: Double?
  public var qp2: Double?
  public var gp2: Double?
  public var fp3: Double?
  public var qp3: Double?
  public var gp3: Double?
  public var fhs: Double?
  public var qhs: Double?
  public var ghs: Double?
  public var freqMin: Double?
  public var freqMax: Double?
  public var gains: [Double]?

  enum CodingKeys: String, CodingKey {
    case type
    case freq, order, gain
    case fls, qls, gls
    case fp1, qp1, gp1
    case fp2, qp2, gp2
    case fp3, qp3, gp3
    case fhs, qhs, ghs
    case freqMin = "freq_min"
    case freqMax = "freq_max"
    case gains
  }

  public init(
    type: BiquadComboType,
    freq: Double? = nil,
    order: Int? = nil,
    gain: Double? = nil,
    fls: Double? = nil, qls: Double? = nil, gls: Double? = nil,
    fp1: Double? = nil, qp1: Double? = nil, gp1: Double? = nil,
    fp2: Double? = nil, qp2: Double? = nil, gp2: Double? = nil,
    fp3: Double? = nil, qp3: Double? = nil, gp3: Double? = nil,
    fhs: Double? = nil, qhs: Double? = nil, ghs: Double? = nil,
    freqMin: Double? = nil, freqMax: Double? = nil,
    gains: [Double]? = nil
  ) {
    self.type = type
    self.freq = freq
    self.order = order
    self.gain = gain
    self.fls = fls
    self.qls = qls
    self.gls = gls
    self.fp1 = fp1
    self.qp1 = qp1
    self.gp1 = gp1
    self.fp2 = fp2
    self.qp2 = qp2
    self.gp2 = gp2
    self.fp3 = fp3
    self.qp3 = qp3
    self.gp3 = gp3
    self.fhs = fhs
    self.qhs = qhs
    self.ghs = ghs
    self.freqMin = freqMin
    self.freqMax = freqMax
    self.gains = gains
  }

  public func validate(sampleRate: Int) throws {
    let nyquist = Double(sampleRate) / 2.0
    switch type {
    case .butterworthLowpass, .butterworthHighpass:
      guard let freq = freq, freq > 0 else {
        throw ConfigError.invalidFilter(
          "BiquadCombo: freq must be > 0, got \(String(describing: freq))")
      }
      guard freq < nyquist else {
        throw ConfigError.invalidFilter(
          "BiquadCombo: freq must be less than Nyquist (\(nyquist)), got \(freq)")
      }
      guard let order = order, order > 0 else {
        throw ConfigError.invalidFilter(
          "BiquadCombo: order must be > 0, got \(String(describing: order))")
      }
    case .linkwitzRileyLowpass, .linkwitzRileyHighpass:
      guard let freq = freq, freq > 0 else {
        throw ConfigError.invalidFilter(
          "BiquadCombo: freq must be > 0, got \(String(describing: freq))")
      }
      guard freq < nyquist else {
        throw ConfigError.invalidFilter(
          "BiquadCombo: freq must be less than Nyquist (\(nyquist)), got \(freq)")
      }
      guard let order = order, order > 0, order % 2 == 0 else {
        throw ConfigError.invalidFilter(
          "Linkwitz-Riley order must be an even non-zero number, got \(String(describing: order))")
      }
    case .tilt:
      guard let gain = gain else {
        throw ConfigError.invalidFilter("Tilt: gain must be set")
      }
      guard gain > -100.0 && gain < 100.0 else {
        throw ConfigError.invalidFilter("Tilt: gain must be between -100 and 100 dB, got \(gain)")
      }
    case .fivePointPeq:
      guard let qls = qls, qls > 0,
        let qhs = qhs, qhs > 0,
        let qp1 = qp1, qp1 > 0,
        let qp2 = qp2, qp2 > 0,
        let qp3 = qp3, qp3 > 0
      else {
        throw ConfigError.invalidFilter("FivePointPeq: all Q-values must be > 0")
      }
      guard let fls = fls, fls < nyquist,
        let fhs = fhs, fhs < nyquist,
        let fp1 = fp1, fp1 < nyquist,
        let fp2 = fp2, fp2 < nyquist,
        let fp3 = fp3, fp3 < nyquist
      else {
        throw ConfigError.invalidFilter(
          "FivePointPeq: all frequencies must be less than Nyquist (\(nyquist))")
      }
      guard fls > 0, fhs > 0, fp1 > 0, fp2 > 0, fp3 > 0 else {
        throw ConfigError.invalidFilter("FivePointPeq: all frequencies must be > 0")
      }
    case .graphicEqualizer:
      guard let gains = gains, !gains.isEmpty else {
        throw ConfigError.invalidFilter("GraphicEqualizer: gains must be non-empty")
      }
      guard let freqMin = freqMin, freqMin > 0,
        let freqMax = freqMax, freqMax > 0
      else {
        throw ConfigError.invalidFilter("GraphicEqualizer: min and max frequencies must be > 0")
      }
      guard freqMin < nyquist, freqMax < nyquist else {
        throw ConfigError.invalidFilter(
          "GraphicEqualizer: min and max frequencies must be less than Nyquist (\(nyquist))")
      }
      guard freqMin < freqMax else {
        throw ConfigError.invalidFilter(
          "GraphicEqualizer: min frequency must be lower than max frequency")
      }
      for g in gains {
        guard g >= -40.0 && g <= 40.0 else {
          throw ConfigError.invalidFilter(
            "GraphicEqualizer: gains must be within +- 40 dB, got \(g)")
        }
      }
    }
  }
}

public struct DiffEqParameters: Codable, Sendable, Equatable {
  public var a: [Double]?
  public var b: [Double]?

  enum CodingKeys: String, CodingKey {
    case a, b
  }

  public init(a: [Double]? = nil, b: [Double]? = nil) {
    self.a = a
    self.b = b
  }

  public func validate() throws {
  }
}

public enum DitherType: String, Codable, Sendable {
  case none = "None"
  case flat = "Flat"
  case highpass = "Highpass"
  case fweighted441 = "Fweighted441"
  case fweightedLong441 = "FweightedLong441"
  case fweightedShort441 = "FweightedShort441"
  case gesemann441 = "Gesemann441"
  case gesemann48 = "Gesemann48"
  case lipshitz441 = "Lipshitz441"
  case lipshitzLong441 = "LipshitzLong441"
  case shibata441 = "Shibata441"
  case shibataHigh441 = "ShibataHigh441"
  case shibataLow441 = "ShibataLow441"
  case shibata48 = "Shibata48"
  case shibataHigh48 = "ShibataHigh48"
  case shibataLow48 = "ShibataLow48"
  case shibata882 = "Shibata882"
  case shibataLow882 = "ShibataLow882"
  case shibata96 = "Shibata96"
  case shibataLow96 = "ShibataLow96"
  case shibata192 = "Shibata192"
  case shibataLow192 = "ShibataLow192"
}

public struct DitherParameters: Codable, Sendable, Equatable {
  public var type: DitherType
  public var bits: Int
  public var amplitude: Double?

  enum CodingKeys: String, CodingKey {
    case type, bits, amplitude
  }

  public init(type: DitherType, bits: Int, amplitude: Double? = nil) {
    self.type = type
    self.bits = bits
    self.amplitude = amplitude
  }

  public func validate() throws {
    guard bits >= 2 else {
      throw ConfigError.invalidFilter("Dither bit depth must be at least 2, got \(bits)")
    }
    if let amplitude = amplitude {
      guard amplitude >= 0 && amplitude <= 100 else {
        throw ConfigError.invalidFilter("Dither amplitude must be in [0, 100], got \(amplitude)")
      }
    }
  }
}

public struct LimiterParameters: Codable, Sendable, Equatable {
  public var clipLimit: Double
  public var softClip: Bool?

  enum CodingKeys: String, CodingKey {
    case clipLimit = "clip_limit"
    case softClip = "soft_clip"
  }

  public init(clipLimit: Double, softClip: Bool? = nil) {
    self.clipLimit = clipLimit
    self.softClip = softClip
  }

  public func validate() throws {
  }
}

public struct LookaheadLimiterParameters: Codable, Sendable, Equatable {
  public var limit: Double
  public var attack: Double
  public var release: Double
  public var unit: DelayUnit?

  enum CodingKeys: String, CodingKey {
    case limit, attack, release, unit
  }

  public init(limit: Double, attack: Double, release: Double, unit: DelayUnit? = nil) {
    self.limit = limit
    self.attack = attack
    self.release = release
    self.unit = unit
  }

  public func validate(sampleRate: Int) throws {
    guard attack >= 0 else {
      throw ConfigError.invalidFilter("Lookahead Limiter: attack cannot be negative, got \(attack)")
    }
    guard release >= 0 else {
      throw ConfigError.invalidFilter(
        "Lookahead Limiter: release cannot be negative, got \(release)")
    }
    let u = unit ?? .ms
    let attackSamples: Double
    switch u {
    case .ms:
      attackSamples = attack / 1000.0 * Double(sampleRate)
    case .us:
      attackSamples = attack / 1_000_000.0 * Double(sampleRate)
    case .samples:
      attackSamples = attack
    case .mm:
      attackSamples = attack / 1000.0 * Double(sampleRate) / 343.0
    }
    guard attackSamples <= Double(sampleRate) else {
      throw ConfigError.invalidFilter(
        "Lookahead Limiter: attack time cannot be longer than 1 second, got \(attackSamples) samples"
      )
    }
  }
}

public struct VolumeParameters: Codable, Sendable, Equatable {
  public var rampTime: Double?
  public var limit: Double?
  public var fader: Fader?

  enum CodingKeys: String, CodingKey {
    case rampTime = "ramp_time"
    case limit
    case fader
  }

  public init(rampTime: Double? = nil, limit: Double? = nil, fader: Fader? = nil) {
    self.rampTime = rampTime
    self.limit = limit
    self.fader = fader
  }

  public func validate() throws {
    if let r = rampTime {
      guard r >= 0 else {
        throw ConfigError.invalidFilter("Volume ramp time cannot be negative, got \(r)")
      }
    }
  }
}
