// Target frequency response for room correction.
//
// A piecewise-linear curve over (frequency, gain) breakpoints. The
// interpolation runs in log-frequency space because that's how human
// hearing — and audio engineering — perceive the spectrum: a "smooth
// downward tilt above 1 kHz" is naturally specified as a couple of
// log-frequency breakpoints, not a polynomial.
//
// Breakpoints outside the data range extrapolate by holding the
// nearest endpoint's value (clamp, not slope). For frequencies
// between two breakpoints, gain is interpolated linearly in
// `(log10(f), gainDB)` space.

import DSPAudio
import Foundation

public struct TargetCurve: Codable, Sendable {
  public struct Breakpoint: Codable, Sendable, Equatable {
    public var freqHz: PrcFmt
    public var gainDB: PrcFmt
    public init(freqHz: PrcFmt, gainDB: PrcFmt) {
      self.freqHz = freqHz
      self.gainDB = gainDB
    }
  }

  /// Breakpoints in ascending frequency order. The class enforces
  /// ordering on every mutation through `setBreakpoints(...)`; direct
  /// access is read-only to avoid invariant breakage.
  public private(set) var breakpoints: [Breakpoint]

  public init(breakpoints: [Breakpoint]) {
    self.breakpoints = breakpoints.sorted { $0.freqHz < $1.freqHz }
  }

  /// Insert a breakpoint, preserving the freq-sorted invariant.
  /// Replaces any existing breakpoint within `mergeToleranceHz` Hz of
  /// `bp.freqHz` (drag-and-snap behaviour for the editor UI).
  public mutating func upsert(_ bp: Breakpoint, mergeToleranceHz: PrcFmt = 1.0) {
    var bps = breakpoints
    if let idx = bps.firstIndex(where: { abs($0.freqHz - bp.freqHz) <= mergeToleranceHz }) {
      bps[idx] = bp
    } else {
      bps.append(bp)
    }
    breakpoints = bps.sorted { $0.freqHz < $1.freqHz }
  }

  /// Evaluate the target curve at `f` Hz. Constant-extrapolates
  /// outside the breakpoint range; piecewise-linear in log-frequency
  /// otherwise. Returns 0 dB for an empty curve.
  public func evaluate(atFreqHz f: PrcFmt) -> PrcFmt {
    if breakpoints.isEmpty { return 0 }
    if f <= breakpoints.first!.freqHz { return breakpoints.first!.gainDB }
    if f >= breakpoints.last!.freqHz { return breakpoints.last!.gainDB }

    // Find the bracketing pair (linear scan; breakpoint counts are
    // tiny — typically < 20 — so a binary search isn't worth the
    // boilerplate).
    var lo = breakpoints[0]
    for next in breakpoints.dropFirst() {
      if f <= next.freqHz {
        let logF = log10(f)
        let logLo = log10(lo.freqHz)
        let logHi = log10(next.freqHz)
        let t = (logF - logLo) / (logHi - logLo)
        return lo.gainDB + t * (next.gainDB - lo.gainDB)
      }
      lo = next
    }
    return breakpoints.last!.gainDB
  }
}

extension TargetCurve {
  /// Flat 0 dB across the band — the natural reference and a reasonable
  /// default for full-range correction.
  public static let flat = TargetCurve(
    breakpoints: [
      Breakpoint(freqHz: 20, gainDB: 0),
      Breakpoint(freqHz: 20_000, gainDB: 0),
    ])

  /// Brüel & Kjær house curve approximation: ~+3 dB at 50 Hz dropping
  /// to 0 dB at 1 kHz, then a downward tilt of ~−1 dB/oct to about
  /// −4 dB at 16 kHz. Captures the "warm + slightly rolled-off"
  /// preference of the average mastering studio.
  public static let bruelKjaer = TargetCurve(
    breakpoints: [
      Breakpoint(freqHz: 20, gainDB: 3.0),
      Breakpoint(freqHz: 50, gainDB: 3.0),
      Breakpoint(freqHz: 1_000, gainDB: 0.0),
      Breakpoint(freqHz: 4_000, gainDB: -2.0),
      Breakpoint(freqHz: 16_000, gainDB: -4.0),
      Breakpoint(freqHz: 20_000, gainDB: -4.0),
    ])

  /// Harman in-room target (Olive 2013): bass shelf ~+4 dB below 80 Hz,
  /// gentle downward tilt above 1 kHz. The published curve is more
  /// nuanced; this is a piecewise-linear approximation good enough for
  /// auto-fit fitting purposes.
  public static let harman = TargetCurve(
    breakpoints: [
      Breakpoint(freqHz: 20, gainDB: 4.5),
      Breakpoint(freqHz: 80, gainDB: 4.5),
      Breakpoint(freqHz: 200, gainDB: 1.5),
      Breakpoint(freqHz: 1_000, gainDB: 0.0),
      Breakpoint(freqHz: 10_000, gainDB: -3.0),
      Breakpoint(freqHz: 20_000, gainDB: -5.5),
    ])

  public enum Preset: String, CaseIterable, Identifiable, Sendable {
    case flat
    case bruelKjaer = "B&K"
    case harman = "Harman"

    public var id: String { rawValue }

    public var curve: TargetCurve {
      switch self {
      case .flat: return .flat
      case .bruelKjaer: return .bruelKjaer
      case .harman: return .harman
      }
    }
  }
}
