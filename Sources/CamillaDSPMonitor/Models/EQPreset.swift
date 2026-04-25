// EQPreset - Biquad EQ preset with multiple parametric bands and CSV import/export

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
  var id: String { rawValue }
  var hasGain: Bool {
    switch self {
    case .peaking, .lowshelf, .highshelf, .lowshelfFO, .highshelfFO: return true
    default: return false
    }
  }
  var hasQ: Bool {
    switch self {
    case .lowpassFO, .highpassFO, .lowshelfFO, .highshelfFO, .allpassFO: return false
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
  }

  let id: UUID
  var type: EQBandType { didSet { invalidateCache() } }
  var freq: Double { didSet { invalidateCache() } }
  var gain: Double { didSet { invalidateCache() } }
  var q: Double { didSet { invalidateCache() } }
  var isEnabled: Bool
  // Cached biquad coefficients — invalidated when band parameters change.
  // Avoids recomputing trig-heavy coefficients on every frequency sample during curve drawing.
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
  enum CodingKeys: String, CodingKey { case id, type, freq, gain, q, isEnabled }
  required init(from decoder: Decoder) throws {
    let c = try decoder.container(keyedBy: CodingKeys.self)
    id = try c.decode(UUID.self, forKey: .id)
    type = try c.decode(EQBandType.self, forKey: .type)
    freq = try c.decode(Double.self, forKey: .freq)
    gain = try c.decode(Double.self, forKey: .gain)
    q = try c.decode(Double.self, forKey: .q)
    isEnabled = try c.decode(Bool.self, forKey: .isEnabled)
  }
  func encode(to encoder: Encoder) throws {
    var c = encoder.container(keyedBy: CodingKeys.self)
    try c.encode(id, forKey: .id)
    try c.encode(type, forKey: .type)
    try c.encode(freq, forKey: .freq)
    try c.encode(gain, forKey: .gain)
    try c.encode(q, forKey: .q)
    try c.encode(isEnabled, forKey: .isEnabled)
  }
  func coefficients(sampleRate: Int) -> BiquadCoefficients? {
    if cachedSampleRate == sampleRate, let cached = cachedCoeffs { return cached }
    let result = BiquadCoefficients.compute(
      type.rawValue, freq: freq, gain: gain, q: q, sampleRate: sampleRate)
    cachedCoeffs = result
    cachedSampleRate = sampleRate
    return result
  }
  func response(atFreq f: Double, sampleRate: Int) -> Double {
    guard isEnabled, let coeffs = coefficients(sampleRate: sampleRate) else { return 0 }
    return gainDB(coeffs: coeffs, f: f, fs: Double(sampleRate))
  }
  private func gainDB(coeffs: BiquadCoefficients, f: Double, fs: Double) -> Double {
    let w = 2.0 * .pi * f / fs
    let cosW = cos(w)
    let sinW = sin(w)
    let cos2W = cos(2.0 * w)
    let sin2W = sin(2.0 * w)
    let numRe = coeffs.b0 + coeffs.b1 * cosW + coeffs.b2 * cos2W
    let numIm = -coeffs.b1 * sinW - coeffs.b2 * sin2W
    let denRe = 1.0 + coeffs.a1 * cosW + coeffs.a2 * cos2W
    let denIm = -coeffs.a1 * sinW - coeffs.a2 * sin2W
    let numMagSq = numRe * numRe + numIm * numIm
    let denMagSq = denRe * denRe + denIm * denIm
    return (denMagSq > 0) ? 10.0 * log10(numMagSq / denMagSq) : 0
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

      // Try AutoEq / EqualizerAPO format: "Filter 1: ON PK Fc 20 Hz Gain -3.0 dB Q 1.41"
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

          // Simple scan
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
      }
      // Try generic CSV: "PK, 1000, -3.0, 1.41"
      else if trimmed.contains(",") {
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
