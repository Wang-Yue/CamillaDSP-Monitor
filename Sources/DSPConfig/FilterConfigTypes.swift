// Standalone filter configuration types.

import Foundation

public enum Fader: String, Sendable, CaseIterable {
  case main = "Main"
  case aux1 = "Aux1"
  case aux2 = "Aux2"
  case aux3 = "Aux3"
  case aux4 = "Aux4"

  public var intValue: Int {
    switch self {
    case .main: return 0
    case .aux1: return 1
    case .aux2: return 2
    case .aux3: return 3
    case .aux4: return 4
    }
  }

  public init?(intValue: Int) {
    switch intValue {
    case 0: self = .main
    case 1: self = .aux1
    case 2: self = .aux2
    case 3: self = .aux3
    case 4: self = .aux4
    default: return nil
    }
  }
}

extension Fader: Codable {
  public init(from decoder: Decoder) throws {
    let container = try decoder.singleValueContainer()
    if let intValue = try? container.decode(Int.self) {
      if let fader = Fader(intValue: intValue) {
        self = fader
        return
      }
    }
    let stringValue = try container.decode(String.self)
    switch stringValue.lowercased() {
    case "main": self = .main
    case "aux1": self = .aux1
    case "aux2": self = .aux2
    case "aux3": self = .aux3
    case "aux4": self = .aux4
    default:
      throw DecodingError.dataCorruptedError(
        in: container,
        debugDescription: "Cannot decode Fader from \(stringValue)"
      )
    }
  }

  public func encode(to encoder: Encoder) throws {
    var container = encoder.singleValueContainer()
    switch self {
    case .main: try container.encode("Main")
    case .aux1: try container.encode("Aux1")
    case .aux2: try container.encode("Aux2")
    case .aux3: try container.encode("Aux3")
    case .aux4: try container.encode("Aux4")
    }
  }
}

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
  case clipper = "Clipper"
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
}

public struct LoudnessParameters: Codable, Sendable, Equatable {
  public var referenceLevel: Double?
  public var highBoost: Double?
  public var lowBoost: Double?
  public var attenuateMid: Bool?
  public var fader: Fader?

  enum CodingKeys: String, CodingKey {
    case referenceLevel = "reference_level"
    case highBoost = "high_boost"
    case lowBoost = "low_boost"
    case attenuateMid = "attenuate_mid"
    case fader
  }

  public init(
    referenceLevel: Double? = nil, highBoost: Double? = nil,
    lowBoost: Double? = nil,
    attenuateMid: Bool? = nil,
    fader: Fader? = nil
  ) {
    self.referenceLevel = referenceLevel
    self.highBoost = highBoost
    self.lowBoost = lowBoost
    self.attenuateMid = attenuateMid
    self.fader = fader
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
  public var qP: Double?
  public var normalizeAtDc: Bool?

  // LinkwitzTransform parameters
  public var freqAct: Double?
  public var qAct: Double?
  public var freqTarget: Double?
  public var qTarget: Double?

  enum CodingKeys: String, CodingKey {
    case type, freq, gain, q, bandwidth, slope
    case a1, a2, b0, b1, b2
    case freqNotch = "freq_z"
    case freqPole = "freq_p"
    case qP = "q_p"
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
    freqNotch: Double? = nil, freqPole: Double? = nil, qP: Double? = nil,
    normalizeAtDc: Bool? = nil,
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
    self.qP = qP
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
  public var skipBytesLines: Int?
  public var readBytesLines: Int?

  enum CodingKeys: String, CodingKey {
    case type, values, filename, format, channel, length
    case skipBytesLines = "skip_bytes_lines"
    case readBytesLines = "read_bytes_lines"
  }

  public init(
    type: ConvType,
    values: [Double]? = nil,
    filename: String? = nil,
    format: String? = nil,
    channel: Int? = nil,
    length: Int? = nil,
    skipBytesLines: Int? = nil,
    readBytesLines: Int? = nil
  ) {
    self.type = type
    self.values = values
    self.filename = filename
    self.format = format
    self.channel = channel
    self.length = length
    self.skipBytesLines = skipBytesLines
    self.readBytesLines = readBytesLines
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
  case clipper(ClipperParameters)
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
    case .clipper: return .clipper
    case .lookaheadLimiter: return .lookaheadLimiter
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
    case .clipper:
      let p = try container.decode(ClipperParameters.self, forKey: .parameters)
      self = .clipper(p)
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
    case .clipper(let p):
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

extension DelayUnit {
  @inlinable
  public func toSamples(delay: Double, sampleRate: Double) -> Double {
    switch self {
    case .ms:
      return delay / 1000.0 * sampleRate
    case .us:
      return delay / 1000000.0 * sampleRate
    case .samples:
      return delay
    case .mm:
      // Speed of sound in air is approx. 343 m/s
      return delay / 1000.0 * sampleRate / 343.0
    }
  }
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
}

public struct ClipperParameters: Codable, Sendable, Equatable {
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
}

public struct LookaheadLimiterParameters: Codable, Sendable, Equatable {
  public var limit: Double
  public var attack: Double
  public var release: Double
  public var attackUnit: DelayUnit?
  public var releaseUnit: DelayUnit?

  enum CodingKeys: String, CodingKey {
    case limit, attack, release
    case attackUnit = "attack_unit"
    case releaseUnit = "release_unit"
  }

  public init(limit: Double, attack: Double, release: Double, attackUnit: DelayUnit? = nil, releaseUnit: DelayUnit? = nil) {
    self.limit = limit
    self.attack = attack
    self.release = release
    self.attackUnit = attackUnit
    self.releaseUnit = releaseUnit
  }
}

public struct VolumeParameters: Codable, Sendable, Equatable {
  public var rampTime: Double?
  public var limit: Double?
  public var fader: Fader?

  enum CodingKeys: String, CodingKey {
    case rampTime = "ramp_time_ms"
    case limit
    case fader
  }

  public init(rampTime: Double? = nil, limit: Double? = nil, fader: Fader? = nil) {
    self.rampTime = rampTime
    self.limit = limit
    self.fader = fader
  }
}
