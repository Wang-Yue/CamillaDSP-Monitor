// EQPreset - Biquad EQ preset with multiple parametric bands and CSV import/export

import DSPConfig
import Foundation
import Observation

public enum EQBandType: String, CaseIterable, Codable, Identifiable, Sendable {
  case peaking = "Peaking"
  case lowshelf = "Lowshelf"
  case highshelf = "Highshelf"
  case lowpass = "Lowpass"
  case highpass = "Highpass"
  case lowpassFO = "LowpassFO"
  case highpassFO = "HighpassFO"
  case lowshelfFO = "LowshelfFO"
  case highshelfFO = "HighshelfFO"
  case notch = "Notch"
  case bandpass = "Bandpass"
  case allpass = "Allpass"
  case allpassFO = "AllpassFO"

  // Advanced biquads incorporated into EQ
  case free = "Free"
  case generalNotch = "GeneralNotch"
  case linkwitzTransform = "LinkwitzTransform"

  public var id: String { rawValue }

  public var hasGain: Bool {
    switch self {
    case .peaking, .lowshelf, .highshelf, .lowshelfFO, .highshelfFO: return true
    default: return false
    }
  }

  public var hasQ: Bool {
    switch self {
    case .lowpassFO, .highpassFO, .lowshelfFO, .highshelfFO, .allpassFO, .free, .generalNotch,
      .linkwitzTransform:
      return false
    default: return true
    }
  }

}

@Observable
public final class EQBand: Identifiable, Codable, Equatable {
  public static func == (lhs: EQBand, rhs: EQBand) -> Bool {
    lhs.id == rhs.id && lhs.type == rhs.type && lhs.freq == rhs.freq && lhs.gain == rhs.gain
      && lhs.q == rhs.q && lhs.isEnabled == rhs.isEnabled
      && lhs.b0 == rhs.b0 && lhs.b1 == rhs.b1 && lhs.b2 == rhs.b2
      && lhs.a1 == rhs.a1 && lhs.a2 == rhs.a2
      && lhs.freqNotch == rhs.freqNotch && lhs.freqPole == rhs.freqPole
      && lhs.qPole == rhs.qPole
      && lhs.normalizeAtDc == rhs.normalizeAtDc
      && lhs.slope == rhs.slope && lhs.bandwidth == rhs.bandwidth
      && lhs.useSlope == rhs.useSlope && lhs.useBandwidth == rhs.useBandwidth
      && lhs.freqAct == rhs.freqAct && lhs.qAct == rhs.qAct
      && lhs.freqTarget == rhs.freqTarget && lhs.qTarget == rhs.qTarget
  }

  public let id: UUID
  public var type: EQBandType { didSet { invalidateCache() } }
  public var freq: Double { didSet { invalidateCache() } }
  public var gain: Double { didSet { invalidateCache() } }
  public var q: Double { didSet { invalidateCache() } }
  public var isEnabled: Bool

  // Free Biquad coefficients
  public var b0: Double = 1.0 { didSet { invalidateCache() } }
  public var b1: Double = 0.0 { didSet { invalidateCache() } }
  public var b2: Double = 0.0 { didSet { invalidateCache() } }
  public var a1: Double = 0.0 { didSet { invalidateCache() } }
  public var a2: Double = 0.0 { didSet { invalidateCache() } }

  // General Notch parameters
  public var freqNotch: Double = 1000.0 { didSet { invalidateCache() } }
  public var freqPole: Double = 1000.0 { didSet { invalidateCache() } }
  public var qPole: Double = 0.707 { didSet { invalidateCache() } }
  public var normalizeAtDc: Bool = true { didSet { invalidateCache() } }

  // Slope / Bandwidth parameters
  public var slope: Double = 6.0 { didSet { invalidateCache() } }
  public var bandwidth: Double = 1.0 { didSet { invalidateCache() } }
  public var useSlope: Bool = false { didSet { invalidateCache() } }
  public var useBandwidth: Bool = false { didSet { invalidateCache() } }

  // Linkwitz Transform parameters
  public var freqAct: Double = 50.0 { didSet { invalidateCache() } }
  public var qAct: Double = 0.707 { didSet { invalidateCache() } }
  public var freqTarget: Double = 20.0 { didSet { invalidateCache() } }
  public var qTarget: Double = 0.707 { didSet { invalidateCache() } }

  // Cached biquad coefficients — invalidated when band parameters change.
  private var cachedCoeffs: BiquadCoefficients?
  private var cachedSampleRate: Int = 0
  private func invalidateCache() {
    cachedCoeffs = nil
    cachedSampleRate = 0
  }

  public init(
    type: EQBandType = .peaking, freq: Double = 1000, gain: Double = 0, q: Double = 0.707,
    isEnabled: Bool = true
  ) {
    self.id = UUID()
    self.type = type
    self.freq = freq
    self.gain = gain
    self.q = q
    self.isEnabled = isEnabled
  }

  public enum CodingKeys: String, CodingKey {
    case id, type, freq, gain, q, isEnabled
    case b0, b1, b2, a1, a2
    case freqNotch, freqPole, qPole, normalizeAtDc
    case slope, bandwidth, useSlope, useBandwidth
    case freqAct, qAct, freqTarget, qTarget
  }

  public required init(from decoder: Decoder) throws {
    let c = try decoder.container(keyedBy: CodingKeys.self)
    id = try c.decode(UUID.self, forKey: .id)
    type = try c.decode(EQBandType.self, forKey: .type)
    freq = try c.decode(Double.self, forKey: .freq)
    gain = try c.decode(Double.self, forKey: .gain)
    q = try c.decode(Double.self, forKey: .q)
    isEnabled = try c.decode(Bool.self, forKey: .isEnabled)

    // Robust decoding of new fields with defaults for backward compatibility
    b0 = try c.decodeIfPresent(Double.self, forKey: .b0) ?? 1.0
    b1 = try c.decodeIfPresent(Double.self, forKey: .b1) ?? 0.0
    b2 = try c.decodeIfPresent(Double.self, forKey: .b2) ?? 0.0
    a1 = try c.decodeIfPresent(Double.self, forKey: .a1) ?? 0.0
    a2 = try c.decodeIfPresent(Double.self, forKey: .a2) ?? 0.0

    freqNotch = try c.decodeIfPresent(Double.self, forKey: .freqNotch) ?? 1000.0
    freqPole = try c.decodeIfPresent(Double.self, forKey: .freqPole) ?? 1000.0
    qPole = try c.decodeIfPresent(Double.self, forKey: .qPole) ?? 0.707
    normalizeAtDc = try c.decodeIfPresent(Bool.self, forKey: .normalizeAtDc) ?? true

    slope = try c.decodeIfPresent(Double.self, forKey: .slope) ?? 6.0
    bandwidth = try c.decodeIfPresent(Double.self, forKey: .bandwidth) ?? 1.0
    useSlope = try c.decodeIfPresent(Bool.self, forKey: .useSlope) ?? false
    useBandwidth = try c.decodeIfPresent(Bool.self, forKey: .useBandwidth) ?? false

    freqAct = try c.decodeIfPresent(Double.self, forKey: .freqAct) ?? 50.0
    qAct = try c.decodeIfPresent(Double.self, forKey: .qAct) ?? 0.707
    freqTarget = try c.decodeIfPresent(Double.self, forKey: .freqTarget) ?? 20.0
    qTarget = try c.decodeIfPresent(Double.self, forKey: .qTarget) ?? 0.707
  }

  public func encode(to encoder: Encoder) throws {
    var c = encoder.container(keyedBy: CodingKeys.self)
    try c.encode(id, forKey: .id)
    try c.encode(type, forKey: .type)
    try c.encode(freq, forKey: .freq)
    try c.encode(gain, forKey: .gain)
    try c.encode(q, forKey: .q)
    try c.encode(isEnabled, forKey: .isEnabled)

    try c.encode(b0, forKey: .b0)
    try c.encode(b1, forKey: .b1)
    try c.encode(b2, forKey: .b2)
    try c.encode(a1, forKey: .a1)
    try c.encode(a2, forKey: .a2)

    try c.encode(freqNotch, forKey: .freqNotch)
    try c.encode(freqPole, forKey: .freqPole)
    try c.encode(qPole, forKey: .qPole)
    try c.encode(normalizeAtDc, forKey: .normalizeAtDc)

    try c.encode(slope, forKey: .slope)
    try c.encode(bandwidth, forKey: .bandwidth)
    try c.encode(useSlope, forKey: .useSlope)
    try c.encode(useBandwidth, forKey: .useBandwidth)

    try c.encode(freqAct, forKey: .freqAct)
    try c.encode(qAct, forKey: .qAct)
    try c.encode(freqTarget, forKey: .freqTarget)
    try c.encode(qTarget, forKey: .qTarget)
  }

  public func coefficients(sampleRate: Int) -> BiquadCoefficients? {
    if cachedSampleRate == sampleRate, let cached = cachedCoeffs { return cached }
    guard let biquadType = BiquadType(rawValue: type.rawValue) else { return nil }

    var params = BiquadParameters(type: biquadType)
    switch type {
    case .free:
      params.b0 = b0
      params.b1 = b1
      params.b2 = b2
      params.a1 = a1
      params.a2 = a2
    case .generalNotch:
      params.freqNotch = freqNotch
      params.freqPole = freqPole
      params.qP = qPole
      params.normalizeAtDc = normalizeAtDc
    case .linkwitzTransform:
      params.freqAct = freqAct
      params.qAct = qAct
      params.freqTarget = freqTarget
      params.qTarget = qTarget
    case .lowshelf, .highshelf:
      params.freq = freq
      params.gain = gain
      if useSlope {
        params.slope = slope
      } else {
        params.q = q
      }
    case .notch, .bandpass, .allpass:
      params.freq = freq
      if useBandwidth {
        params.bandwidth = bandwidth
      } else {
        params.q = q
      }
    default:
      params.freq = freq
      params.gain = type.hasGain ? gain : nil
      params.q = type.hasQ ? q : nil
    }

    let result = BiquadCoefficients.compute(parameters: params, sampleRate: sampleRate)
    cachedCoeffs = result
    cachedSampleRate = sampleRate
    return result
  }

  public func response(atFreq f: Double, sampleRate: Int) -> Double {
    guard isEnabled, let coeffs = coefficients(sampleRate: sampleRate) else { return 0 }
    return coeffs.gainDB(atFreqHz: f, sampleRate: sampleRate)
  }

  public func phaseResponse(atFreq f: Double, sampleRate: Int) -> Double {
    guard isEnabled, let coeffs = coefficients(sampleRate: sampleRate) else { return 0 }
    return coeffs.phaseRad(atFreqHz: f, sampleRate: sampleRate)
  }
}

@Observable
public final class EQPreset: Identifiable, Codable, Equatable {
  public static func == (lhs: EQPreset, rhs: EQPreset) -> Bool {
    lhs.id == rhs.id && lhs.name == rhs.name && lhs.preampGain == rhs.preampGain
      && lhs.bands == rhs.bands
  }

  public let id: UUID
  public var name: String
  public var preampGain: Double
  public var bands: [EQBand]

  public init(name: String, preampGain: Double = -6.0, bands: [EQBand] = []) {
    self.id = UUID()
    self.name = name
    self.preampGain = preampGain
    self.bands = bands
  }

  public enum CodingKeys: String, CodingKey { case id, name, preampGain, bands }

  public required init(from decoder: Decoder) throws {
    let c = try decoder.container(keyedBy: CodingKeys.self)
    id = try c.decode(UUID.self, forKey: .id)
    name = try c.decode(String.self, forKey: .name)
    preampGain = try c.decodeIfPresent(Double.self, forKey: .preampGain) ?? -6.0
    bands = try c.decode([EQBand].self, forKey: .bands)
  }

  public func encode(to encoder: Encoder) throws {
    var c = encoder.container(keyedBy: CodingKeys.self)
    try c.encode(id, forKey: .id)
    try c.encode(name, forKey: .name)
    try c.encode(preampGain, forKey: .preampGain)
    try c.encode(bands, forKey: .bands)
  }

  public func addBand(_ band: EQBand? = nil) { bands.append(band ?? EQBand()) }
  public func removeBand(at index: Int) {
    if bands.indices.contains(index) { bands.remove(at: index) }
  }

  public func combinedResponse(atFreq f: Double, sampleRate: Int) -> Double {
    preampGain
      + bands.filter(\.isEnabled).reduce(0.0) {
        $0 + $1.response(atFreq: f, sampleRate: sampleRate)
      }
  }

  public func combinedPhase(atFreq f: Double, sampleRate: Int) -> Double {
    bands.filter(\.isEnabled).reduce(0.0) {
      $0 + $1.phaseResponse(atFreq: f, sampleRate: sampleRate)
    }
  }

}
