// TargetCurve interpolation correctness tests.
//
// The curve is piecewise-linear in log-frequency space, so the
// expected midpoint between (f1, g1) and (f2, g2) at frequency
// `√(f1·f2)` is exactly `(g1 + g2) / 2`.

import Foundation
import Testing

@testable import DSPAudio
@testable import DSPMeasurement

@Suite struct TargetCurveTests {

  @Test func EmptyCurveReturnsZero() {
    let curve = TargetCurve(breakpoints: [])
    #expect(curve.evaluate(atFreqHz: 100) == 0)
    #expect(curve.evaluate(atFreqHz: 10_000) == 0)
  }

  @Test func ConstantExtrapolationOutsideRange() {
    let curve = TargetCurve(breakpoints: [
      .init(freqHz: 100, gainDB: -3),
      .init(freqHz: 10_000, gainDB: 6),
    ])
    // Below the lowest breakpoint, the curve clamps to the first gain.
    #expect(curve.evaluate(atFreqHz: 20) == -3)
    // Above the highest breakpoint, clamps to the last gain.
    #expect(curve.evaluate(atFreqHz: 25_000) == 6)
  }

  @Test func ExactBreakpointReturnsBreakpointGain() {
    let curve = TargetCurve(breakpoints: [
      .init(freqHz: 100, gainDB: -3),
      .init(freqHz: 1000, gainDB: 0),
      .init(freqHz: 10_000, gainDB: 6),
    ])
    #expect(curve.evaluate(atFreqHz: 100) == -3)
    #expect(curve.evaluate(atFreqHz: 1000) == 0)
    #expect(curve.evaluate(atFreqHz: 10_000) == 6)
  }

  /// Geometric midpoint of two breakpoints should land exactly at the
  /// gain midpoint, since interpolation is in log10(f).
  @Test func LogMidpointIsGainMidpoint() {
    let curve = TargetCurve(breakpoints: [
      .init(freqHz: 100, gainDB: 0),
      .init(freqHz: 10_000, gainDB: 12),
    ])
    let geomMid = sqrt(100 * 10_000.0)  // = 1000
    #expect(abs(curve.evaluate(atFreqHz: geomMid) - 6.0) < 1e-9)
  }

  @Test func PresetsLandWithinReasonableBounds() {
    // The presets aren't formally specified to a tight tolerance, but
    // they should sit inside the audio band and give bounded gains.
    for preset in TargetCurve.Preset.allCases {
      let curve = preset.curve
      for f in [20.0, 100.0, 1000.0, 10_000.0, 20_000.0] as [PrcFmt] {
        let g = curve.evaluate(atFreqHz: f)
        #expect(g >= -10 && g <= 10, "\(preset) at \(f) Hz = \(g) dB out of bounds")
      }
    }
  }

  @Test func UpsertReplacesNearbyBreakpoint() {
    var curve = TargetCurve(breakpoints: [
      .init(freqHz: 100, gainDB: 0),
      .init(freqHz: 1000, gainDB: 5),
    ])
    // Within tolerance — should replace, not insert.
    curve.upsert(.init(freqHz: 100.5, gainDB: 3), mergeToleranceHz: 1.0)
    #expect(curve.breakpoints.count == 2)
    #expect(curve.breakpoints[0].gainDB == 3)
    // Outside tolerance — should insert a new breakpoint.
    curve.upsert(.init(freqHz: 500, gainDB: 2), mergeToleranceHz: 1.0)
    #expect(curve.breakpoints.count == 3)
    #expect(curve.breakpoints.map(\.freqHz) == [100.5, 500, 1000])
  }
}
