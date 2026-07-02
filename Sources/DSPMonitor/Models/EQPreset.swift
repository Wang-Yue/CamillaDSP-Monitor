// EQPreset - Biquad EQ preset with multiple parametric bands and CSV import/export

import DSPConfig
import DSPFilters
import Foundation
import Observation

enum EQBandType: String, CaseIterable, Codable, Identifiable {
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

  var id: String { rawValue }

  var isStandard: Bool {
    switch self {
    case .free, .generalNotch, .linkwitzTransform: return false
    default: return true
    }
  }

  var hasGain: Bool {
    switch self {
    case .peaking, .lowshelf, .highshelf, .lowshelfFO, .highshelfFO: return true
    default: return false
    }
  }

  var hasQ: Bool {
    switch self {
    case .lowpassFO, .highpassFO, .lowshelfFO, .highshelfFO, .allpassFO, .free, .generalNotch,
      .linkwitzTransform:
      return false
    default: return true
    }
  }

  private static let shortNameMap: [String: EQBandType] = [
    "PK": .peaking, "LS": .lowshelf, "HS": .highshelf, "LP": .lowpass,
    "HP": .highpass, "NO": .notch, "BP": .bandpass, "AP": .allpass,
    "LSC": .lowshelf, "HSC": .highshelf,
  ]
  var shortName: String { Self.shortNameMap.first(where: { $0.value == self })?.key ?? rawValue }
  static func fromShortName(_ s: String) -> EQBandType {
    shortNameMap[s.uppercased()] ?? .peaking
  }
}

@Observable
final class EQBand: Identifiable, Codable, Equatable {
  static func == (lhs: EQBand, rhs: EQBand) -> Bool {
    lhs.id == rhs.id && lhs.type == rhs.type && lhs.freq == rhs.freq && lhs.gain == rhs.gain
      && lhs.q == rhs.q && lhs.isEnabled == rhs.isEnabled
      && lhs.b0 == rhs.b0 && lhs.b1 == rhs.b1 && lhs.b2 == rhs.b2
      && lhs.a1 == rhs.a1 && lhs.a2 == rhs.a2
      && lhs.freqNotch == rhs.freqNotch && lhs.freqPole == rhs.freqPole
      && lhs.qPole == rhs.qPole
      && lhs.normalizeAtDc == rhs.normalizeAtDc
      && lhs.freqAct == rhs.freqAct && lhs.qAct == rhs.qAct
      && lhs.freqTarget == rhs.freqTarget && lhs.qTarget == rhs.qTarget
  }

  let id: UUID
  var type: EQBandType { didSet { invalidateCache() } }
  var freq: Double { didSet { invalidateCache() } }
  var gain: Double { didSet { invalidateCache() } }
  var q: Double { didSet { invalidateCache() } }
  var isEnabled: Bool

  // Free Biquad coefficients
  var b0: Double = 1.0 { didSet { invalidateCache() } }
  var b1: Double = 0.0 { didSet { invalidateCache() } }
  var b2: Double = 0.0 { didSet { invalidateCache() } }
  var a1: Double = 0.0 { didSet { invalidateCache() } }
  var a2: Double = 0.0 { didSet { invalidateCache() } }

  // General Notch parameters
  var freqNotch: Double = 1000.0 { didSet { invalidateCache() } }
  var freqPole: Double = 1000.0 { didSet { invalidateCache() } }
  var qPole: Double = 0.707 { didSet { invalidateCache() } }
  var normalizeAtDc: Bool = true { didSet { invalidateCache() } }

  // Linkwitz Transform parameters
  var freqAct: Double = 50.0 { didSet { invalidateCache() } }
  var qAct: Double = 0.707 { didSet { invalidateCache() } }
  var freqTarget: Double = 20.0 { didSet { invalidateCache() } }
  var qTarget: Double = 0.707 { didSet { invalidateCache() } }

  // Cached biquad coefficients — invalidated when band parameters change.
  private var cachedCoeffs: BiquadCoefficients?
  private var cachedSampleRate: Int = 0
  private func invalidateCache() {
    cachedCoeffs = nil
    cachedSampleRate = 0
  }

  init(
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

  enum CodingKeys: String, CodingKey {
    case id, type, freq, gain, q, isEnabled
    case b0, b1, b2, a1, a2
    case freqNotch, freqPole, qPole, normalizeAtDc
    case freqAct, qAct, freqTarget, qTarget
  }

  required init(from decoder: Decoder) throws {
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

    freqAct = try c.decodeIfPresent(Double.self, forKey: .freqAct) ?? 50.0
    qAct = try c.decodeIfPresent(Double.self, forKey: .qAct) ?? 0.707
    freqTarget = try c.decodeIfPresent(Double.self, forKey: .freqTarget) ?? 20.0
    qTarget = try c.decodeIfPresent(Double.self, forKey: .qTarget) ?? 0.707
  }

  func encode(to encoder: Encoder) throws {
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

    try c.encode(freqAct, forKey: .freqAct)
    try c.encode(qAct, forKey: .qAct)
    try c.encode(freqTarget, forKey: .freqTarget)
    try c.encode(qTarget, forKey: .qTarget)
  }

  func coefficients(sampleRate: Int) -> BiquadCoefficients? {
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

  func response(atFreq f: Double, sampleRate: Int) -> Double {
    guard isEnabled, let coeffs = coefficients(sampleRate: sampleRate) else { return 0 }
    return coeffs.gainDB(atFreqHz: f, sampleRate: sampleRate)
  }

  func phaseResponse(atFreq f: Double, sampleRate: Int) -> Double {
    guard isEnabled, let coeffs = coefficients(sampleRate: sampleRate) else { return 0 }
    return coeffs.phaseRad(atFreqHz: f, sampleRate: sampleRate)
  }
}

@Observable
final class EQPreset: Identifiable, Codable, Equatable {
  static func == (lhs: EQPreset, rhs: EQPreset) -> Bool {
    lhs.id == rhs.id && lhs.name == rhs.name && lhs.preampGain == rhs.preampGain
      && lhs.bands == rhs.bands
  }

  let id: UUID
  var name: String
  var preampGain: Double
  var bands: [EQBand]
  init(name: String, preampGain: Double = -6.0, bands: [EQBand] = []) {
    self.id = UUID()
    self.name = name
    self.preampGain = preampGain
    self.bands = bands
  }
  enum CodingKeys: String, CodingKey { case id, name, preampGain, bands }
  required init(from decoder: Decoder) throws {
    let c = try decoder.container(keyedBy: CodingKeys.self)
    id = try c.decode(UUID.self, forKey: .id)
    name = try c.decode(String.self, forKey: .name)
    preampGain = try c.decodeIfPresent(Double.self, forKey: .preampGain) ?? -6.0
    bands = try c.decode([EQBand].self, forKey: .bands)
  }
  func encode(to encoder: Encoder) throws {
    var c = encoder.container(keyedBy: CodingKeys.self)
    try c.encode(id, forKey: .id)
    try c.encode(name, forKey: .name)
    try c.encode(preampGain, forKey: .preampGain)
    try c.encode(bands, forKey: .bands)
  }
  func addBand(_ band: EQBand? = nil) { bands.append(band ?? EQBand()) }
  func removeBand(at index: Int) { if bands.indices.contains(index) { bands.remove(at: index) } }
  func combinedResponse(atFreq f: Double, sampleRate: Int) -> Double {
    preampGain
      + bands.filter(\.isEnabled).reduce(0.0) {
        $0 + $1.response(atFreq: f, sampleRate: sampleRate)
      }
  }

  func combinedPhase(atFreq f: Double, sampleRate: Int) -> Double {
    bands.filter(\.isEnabled).reduce(0.0) {
      $0 + $1.phaseResponse(atFreq: f, sampleRate: sampleRate)
    }
  }

  // MARK: - AutoEq / EqualizerAPO CSV Format

  func toCSV() -> String {
    var lines = ["Preamp: \(String(format: "%.1f", preampGain)) dB"]
    for (i, band) in bands.enumerated() {
      let state = band.isEnabled ? "ON" : "OFF"
      let type = band.type.shortName
      let gainStr = band.type.hasGain ? "Gain \(String(format: "%.1f", band.gain)) dB " : ""
      let qStr = band.type.hasQ ? "Q \(String(format: "%.2f", band.q))" : ""
      lines.append("Filter \(i + 1): \(state) \(type) Fc \(Int(band.freq)) Hz \(gainStr)\(qStr)")
    }
    return lines.joined(separator: "\n")
  }

  static func fromCSV(_ text: String) -> (preamp: Double, bands: [EQBand])? {
    var bands: [EQBand] = []
    var preamp = 0.0
    let lines = text.components(separatedBy: .newlines)

    for line in lines {
      let trimmed = line.trimmingCharacters(in: .whitespaces)
      if trimmed.isEmpty || trimmed.hasPrefix("#") { continue }

      if trimmed.lowercased().hasPrefix("preamp:") {
        let parts = trimmed.components(separatedBy: ":")
        if parts.count > 1 {
          let valStr = parts[1].replacingOccurrences(of: "dB", with: "").trimmingCharacters(
            in: .whitespaces)
          preamp = Double(valStr) ?? 0.0
        }
        continue
      }

      if trimmed.lowercased().hasPrefix("filter") {
        let content =
          trimmed.components(separatedBy: ":").last?.trimmingCharacters(in: .whitespaces) ?? ""
        let words = content.components(separatedBy: .whitespaces).filter { !$0.isEmpty }
        if words.count >= 4 {
          let isEnabled = words[0].uppercased() == "ON"
          let type = EQBandType.fromShortName(words[1])

          var freq = 1000.0
          var gain = 0.0
          var q = 0.707

          for i in 0..<words.count - 1 {
            let key = words[i].lowercased()
            let val = Double(words[i + 1]) ?? 0.0
            if key == "fc" {
              freq = val
            } else if key == "gain" {
              gain = val
            } else if key == "q" {
              q = val
            }
          }
          bands.append(EQBand(type: type, freq: freq, gain: gain, q: q, isEnabled: isEnabled))
        }
      } else if trimmed.contains(",") {
        let parts = trimmed.components(separatedBy: ",").map {
          $0.trimmingCharacters(in: .whitespaces)
        }
        if parts.count >= 2 {
          let type = EQBandType.fromShortName(parts[0])
          let freq = Double(parts[1]) ?? 1000.0
          let gain = parts.count > 2 ? (Double(parts[2]) ?? 0.0) : 0.0
          let q = parts.count > 3 ? (Double(parts[3]) ?? 0.707) : 0.707
          bands.append(EQBand(type: type, freq: freq, gain: gain, q: q))
        }
      }
    }

    return bands.isEmpty ? nil : (preamp, bands)
  }
}
