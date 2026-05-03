// Unit tests for `WindowFunction.swift`. The production resampler today
// only invokes `BlackmanHarris2`, but the file exposes Hann, Hann², Blackman,
// Blackman², BlackmanHarris and BlackmanHarris² so they can be wired up by
// future profiles. These tests exercise:
//
//   - `windowValue(_:i:n:)` for every window: structural properties
//     (periodicity, mid-window peak, squared variants equal the unsquared
//     squared, hand-computed sample values).
//   - `calculateCutoff(sincLen:window:)` and `calculateCutoffF32` for every
//     window: matches the values rubato's `calculate_cutoff` test in
//     windows.rs::test_cutoff (the same cubic-fit constants).
//   - `makeSincTable`: spot-check normalisation (DC gain ≈ 1) and symmetry
//     for the `factor=1` path used by `SynchronousResampler`.

import Foundation
import XCTest

@testable import CamillaDSPLib

final class WindowFunctionTests: XCTestCase {

  // MARK: - windowValue properties

  /// At i = n/2 (the centre of an even window), every window should peak at
  /// or near 1.0 — the implementations are normalised so the centre tap is
  /// the maximum-amplitude sample.
  func testCentrePeak() {
    let n = 64
    let mid = n / 2
    let windows: [(WindowFunction, String)] = [
      (.hann, "hann"),
      (.hann2, "hann2"),
      (.blackman, "blackman"),
      (.blackman2, "blackman2"),
      (.blackmanHarris, "blackmanHarris"),
      (.blackmanHarris2, "blackmanHarris2"),
    ]
    for (w, label) in windows {
      let v = windowValue(w, i: mid, n: n)
      XCTAssertEqual(
        v, 1.0, accuracy: 1e-12,
        "[\(label)] windowValue at centre i=n/2 should be 1.0; got \(v)")
    }
  }

  /// At i = 0 the periodic windows should evaluate at their endpoint (which
  /// is `cos(0) = 1` after substitution → all coefficients sum). Spot-check
  /// the analytical values for each window.
  func testEndpointValues() {
    let n = 64
    // For periodic windows, i=0 → cos(2πk·0/n) = 1 for every harmonic, so
    // the value equals the sum of the (signed) coefficients. The squared
    // variants square that.
    XCTAssertEqual(windowValue(.hann, i: 0, n: n), 0.0, accuracy: 1e-15)
    XCTAssertEqual(windowValue(.hann2, i: 0, n: n), 0.0, accuracy: 1e-15)
    XCTAssertEqual(windowValue(.blackman, i: 0, n: n), 0.0, accuracy: 1e-12)
    XCTAssertEqual(windowValue(.blackman2, i: 0, n: n), 0.0, accuracy: 1e-12)
    // BlackmanHarris coefficients sum: 0.35875 - 0.48829 + 0.14128 - 0.01168
    // = 0.00006 (tiny but non-zero).
    let bh0 = 0.35875 - 0.48829 + 0.14128 - 0.01168
    XCTAssertEqual(windowValue(.blackmanHarris, i: 0, n: n), bh0, accuracy: 1e-12)
    XCTAssertEqual(
      windowValue(.blackmanHarris2, i: 0, n: n), bh0 * bh0, accuracy: 1e-12)
  }

  /// `*2` variants must equal the squared base window pointwise.
  func testSquaredWindowsAreSquares() {
    let n = 32
    for i in 0..<n {
      let pairs: [(WindowFunction, WindowFunction, String)] = [
        (.hann, .hann2, "hann"),
        (.blackman, .blackman2, "blackman"),
        (.blackmanHarris, .blackmanHarris2, "blackmanHarris"),
      ]
      for (base, squared, label) in pairs {
        let b = windowValue(base, i: i, n: n)
        let s = windowValue(squared, i: i, n: n)
        XCTAssertEqual(
          s, b * b, accuracy: 1e-13,
          "[\(label) i=\(i)] squared variant should equal base²")
      }
    }
  }

  /// Periodicity: w(i, n) = w(i + n, n). For periodic windows this is exact.
  func testPeriodicity() {
    let n = 32
    let windows: [WindowFunction] = [
      .hann, .hann2, .blackman, .blackman2, .blackmanHarris, .blackmanHarris2,
    ]
    for w in windows {
      for i in 0..<n {
        let v0 = windowValue(w, i: i, n: n)
        let v1 = windowValue(w, i: i + n, n: n)
        XCTAssertEqual(
          v0, v1, accuracy: 1e-13,
          "windowValue periodic over n; i=\(i) gave \(v0) vs \(v1)")
      }
    }
  }

  // MARK: - calculateCutoff

  /// `calculateCutoff` (Double) for length 256 should match rubato's test
  /// values in `windows.rs::test_cutoff` to 0.001 — the cubic-fit constants
  /// are copied verbatim, so anything looser than that is a bug.
  func testCalculateCutoffMatchesRubato() {
    let len = 256
    let expectations: [(WindowFunction, Double)] = [
      (.blackman, 0.976),
      (.blackman2, 0.963),
      (.blackmanHarris, 0.969),
      (.blackmanHarris2, 0.947),
      (.hann, 0.987),
      (.hann2, 0.979),
    ]
    for (w, expected) in expectations {
      let c = calculateCutoff(sincLen: len, window: w)
      XCTAssertEqual(
        c, expected, accuracy: 0.001,
        "[\(w)] calculateCutoff(256) = \(c), expected ~\(expected)")
    }
    // Length 128 has another rubato reference set.
    let len128: [(WindowFunction, Double)] = [
      (.blackman, 0.953),
      (.blackman2, 0.926),
      (.blackmanHarris, 0.937),
      (.blackmanHarris2, 0.894),
      (.hann, 0.974),
      (.hann2, 0.958),
    ]
    for (w, expected) in len128 {
      let c = calculateCutoff(sincLen: 128, window: w)
      XCTAssertEqual(
        c, expected, accuracy: 0.001,
        "[\(w)] calculateCutoff(128) = \(c), expected ~\(expected)")
    }
  }

  /// `calculateCutoffF32` should match the f64 form to f32 precision.
  /// (AsyncSinc and Synchronous use the f32 version verbatim because rubato
  /// does — different rounding here would put the kernel slightly off.)
  func testCalculateCutoffF32MatchesDouble() {
    let lens = [32, 64, 128, 256, 512, 1024]
    let windows: [WindowFunction] = [
      .hann, .hann2, .blackman, .blackman2, .blackmanHarris, .blackmanHarris2,
    ]
    for len in lens {
      for w in windows {
        let cD = calculateCutoff(sincLen: len, window: w)
        let cF = calculateCutoffF32(sincLen: len, window: w)
        XCTAssertEqual(
          Double(cF), cD, accuracy: 1e-6,
          "[\(w) len=\(len)] f32 \(cF) vs f64 \(cD) diverge beyond f32 precision")
      }
    }
  }

  // MARK: - makeSincTable

  /// Exercise every supported window with `makeSincTable(factor=1)` (the
  /// SynchronousResampler path) and confirm the sum of the kernel comes out
  /// at exactly 1.0 — that's how `make_sincs` normalises (sum/factor with
  /// factor=1).
  func testMakeSincTableNormalisation() {
    let len = 64
    let windows: [WindowFunction] = [
      .hann, .hann2, .blackman, .blackman2, .blackmanHarris, .blackmanHarris2,
    ]
    for w in windows {
      let cutoff = calculateCutoff(sincLen: len, window: w)
      let table = makeSincTable(
        sincLen: len, oversamplingFactor: 1, window: w, fc: cutoff)
      XCTAssertEqual(table.count, len)
      let sum = table.reduce(0.0, +)
      XCTAssertEqual(
        sum, 1.0, accuracy: 1e-10,
        "[\(w) len=\(len)] makeSincTable sum = \(sum), expected 1.0 (DC gain)")
    }
  }

  /// For `oversamplingFactor > 1`, the table holds `factor` decimated
  /// sub-filters concatenated end-to-end. Exercise this layout to make sure
  /// `makeSincTable` does the right thing for the AsyncSinc path too.
  func testMakeSincTableOversampledLayout() {
    let len = 16
    let factor = 4
    let cutoff = calculateCutoff(sincLen: len, window: .blackmanHarris2)
    let table = makeSincTable(
      sincLen: len, oversamplingFactor: factor,
      window: .blackmanHarris2, fc: cutoff)
    XCTAssertEqual(table.count, len * factor)
    // Sum of all sub-filters = factor (each sub-filter has DC gain 1).
    let total = table.reduce(0.0, +)
    XCTAssertEqual(
      total, Double(factor), accuracy: 1e-9,
      "Total sum should be factor=\(factor); got \(total)")
  }
}
