// Convolution preset — a saved reference to a *family* of impulse
// response files on disk, one per common sample rate. The biquad
// chain underlying the FIR is the same; only the discretisation
// changes per rate. The engine picks the IR matching the live
// capture rate at config-build time so no resampling of the
// correction is needed at runtime.
//
// Files live alongside other measurement output in
// `~/Library/Application Support/DSPMonitor/IRs/` as raw
// little-endian Doubles (matches `format: F64_LE`).
//
// On disk: `[Int: String]` keyed by sample rate (Hz) → absolute path.
// The Codable accepts a legacy `irPath: String + sampleRate: Int`
// shape from earlier versions, so previously-saved presets keep
// working.

import Foundation
import Observation

@Observable
public final class ConvolutionPreset: Identifiable, Codable, Equatable {
  public static func == (lhs: ConvolutionPreset, rhs: ConvolutionPreset) -> Bool {
    lhs.id == rhs.id
      && lhs.name == rhs.name
      && lhs.irPaths == rhs.irPaths
      && lhs.taps == rhs.taps
      && lhs.kindLabel == rhs.kindLabel
  }

  public let id: UUID
  public var name: String

  /// Map from sample rate (Hz) → absolute IR file path. Engine picks
  /// the entry matching the live capture rate; the UI defaults to
  /// the closest match when previewing.
  public var irPaths: [Int: String]

  /// Tap count of the IR (same across all rates — the design FFT
  /// length is rate-independent in the current FIR routines).
  public var taps: Int

  /// Display label for the design kind ("Min-phase", "Linear-phase",
  /// "Imported", …). Free-form so future kinds don't need migration.
  public var kindLabel: String

  public init(name: String, irPaths: [Int: String], taps: Int, kindLabel: String) {
    self.id = UUID()
    self.name = name
    self.irPaths = irPaths
    self.taps = taps
    self.kindLabel = kindLabel
  }

  public enum CodingKeys: String, CodingKey {
    case id, name, irPaths, taps, kindLabel
    // Legacy single-rate fields (still accepted on decode).
    case irPath, sampleRate
  }

  public required init(from decoder: Decoder) throws {
    let c = try decoder.container(keyedBy: CodingKeys.self)
    id = try c.decode(UUID.self, forKey: .id)
    name = try c.decode(String.self, forKey: .name)
    taps = try c.decode(Int.self, forKey: .taps)
    kindLabel = try c.decode(String.self, forKey: .kindLabel)

    // Prefer the new dictionary; fall back to single-rate legacy
    // form so previously-saved presets keep working.
    if let paths = try c.decodeIfPresent([String: String].self, forKey: .irPaths) {
      var byRate: [Int: String] = [:]
      for (k, v) in paths {
        if let r = Int(k) { byRate[r] = v }
      }
      irPaths = byRate
    } else if let oldPath = try c.decodeIfPresent(String.self, forKey: .irPath),
      let oldRate = try c.decodeIfPresent(Int.self, forKey: .sampleRate)
    {
      irPaths = [oldRate: oldPath]
    } else {
      irPaths = [:]
    }
  }

  public func encode(to encoder: Encoder) throws {
    var c = encoder.container(keyedBy: CodingKeys.self)
    try c.encode(id, forKey: .id)
    try c.encode(name, forKey: .name)
    try c.encode(taps, forKey: .taps)
    try c.encode(kindLabel, forKey: .kindLabel)
    // Encode dictionary with stringified Int keys (JSON requires it).
    var stringMap: [String: String] = [:]
    for (k, v) in irPaths { stringMap[String(k)] = v }
    try c.encode(stringMap, forKey: .irPaths)
  }

  /// Pick the IR path for a given sample rate. Returns the exact
  /// match if available, otherwise the file whose rate is closest in
  /// log-space (since common audio rates cluster around powers and
  /// half-rates of 44.1k / 48k families).
  public func irPath(forSampleRate rate: Int) -> String? {
    if let exact = irPaths[rate] { return exact }
    let available = irPaths.keys.sorted()
    guard !available.isEmpty else { return nil }
    let target = log(Double(rate))
    var bestRate = available[0]
    var bestDelta = abs(log(Double(bestRate)) - target)
    for r in available.dropFirst() {
      let d = abs(log(Double(r)) - target)
      if d < bestDelta {
        bestDelta = d
        bestRate = r
      }
    }
    return irPaths[bestRate]
  }

}
