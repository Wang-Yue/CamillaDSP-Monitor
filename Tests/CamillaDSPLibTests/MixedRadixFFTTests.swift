// Unit tests for `MixedRadixFFT`. Verifies the implementation against an
// O(N²) direct DFT for small sizes, exercises every supported radix
// (2/3/5/7), checks round-trip identity, and confirms the `nil`-on-prime
// guard for unsupported factors.

import Foundation
import XCTest

@testable import CamillaDSPLib

final class MixedRadixFFTTests: XCTestCase {

  // MARK: - Reference DFT

  /// Direct O(N²) complex DFT, used as the ground truth.
  private func directDFT(
    realIn: [Double], imagIn: [Double], inverse: Bool
  ) -> (re: [Double], im: [Double]) {
    let n = realIn.count
    var re = [Double](repeating: 0, count: n)
    var im = [Double](repeating: 0, count: n)
    let sign: Double = inverse ? 1.0 : -1.0
    for k in 0..<n {
      var sumR = 0.0
      var sumI = 0.0
      for nn in 0..<n {
        let theta = sign * 2.0 * .pi * Double(nn * k) / Double(n)
        let cR = cos(theta)
        let cI = sin(theta)
        sumR += realIn[nn] * cR - imagIn[nn] * cI
        sumI += realIn[nn] * cI + imagIn[nn] * cR
      }
      re[k] = sumR
      im[k] = sumI
    }
    return (re, im)
  }

  // MARK: - Helpers

  private func runMixedRadix(
    realIn: [Double], imagIn: [Double], inverse: Bool
  ) -> (re: [Double], im: [Double]) {
    let n = realIn.count
    guard let fft = MixedRadixFFT(n: n) else {
      XCTFail("MixedRadixFFT(n: \(n)) returned nil")
      return ([], [])
    }
    var realOut = [Double](repeating: 0, count: n)
    var imagOut = [Double](repeating: 0, count: n)
    realIn.withUnsafeBufferPointer { rIn in
      imagIn.withUnsafeBufferPointer { iIn in
        realOut.withUnsafeMutableBufferPointer { rOut in
          imagOut.withUnsafeMutableBufferPointer { iOut in
            fft.execute(
              realIn: rIn.baseAddress!, imagIn: iIn.baseAddress!,
              realOut: rOut.baseAddress!, imagOut: iOut.baseAddress!,
              inverse: inverse)
          }
        }
      }
    }
    return (realOut, imagOut)
  }

  /// Element-wise max-abs difference between two complex vectors.
  private func maxAbsDiff(
    _ a: (re: [Double], im: [Double]),
    _ b: (re: [Double], im: [Double])
  ) -> Double {
    var maxDiff = 0.0
    for i in 0..<a.re.count {
      let dR = abs(a.re[i] - b.re[i])
      let dI = abs(a.im[i] - b.im[i])
      if dR > maxDiff { maxDiff = dR }
      if dI > maxDiff { maxDiff = dI }
    }
    return maxDiff
  }

  /// Pseudo-random complex vector for a given seed and length.
  private func randomComplex(n: Int, seed: UInt64) -> (re: [Double], im: [Double]) {
    var rng = SplitMix64(seed: seed)
    var re = [Double](repeating: 0, count: n)
    var im = [Double](repeating: 0, count: n)
    for i in 0..<n {
      re[i] = rng.nextUnit() * 2.0 - 1.0
      im[i] = rng.nextUnit() * 2.0 - 1.0
    }
    return (re, im)
  }

  // MARK: - Tests

  /// Each supported radix in isolation (factor list = single prime).
  func testRadixIsolated() throws {
    for n in [2, 3, 4, 5, 7, 8, 9] {
      try assertMatchesDirectDFT(n: n, label: "isolated radix N=\(n)")
    }
  }

  /// Composite sizes that exercise the radix combination paths.
  func testCompositeSizes() throws {
    for n in [6, 10, 12, 14, 15, 16, 21, 25, 35, 49, 64, 105, 147, 343, 1029, 1120] {
      try assertMatchesDirectDFT(n: n, label: "composite N=\(n)")
    }
  }

  /// Forward + inverse should recover the input scaled by N (matches realfft
  /// convention — both transforms are unnormalised).
  func testRoundTrip() throws {
    for n in [3, 5, 7, 14, 21, 1029, 1120] {
      let input = randomComplex(n: n, seed: UInt64(n) &* 0x9E37_79B9_7F4A_7C15)
      let forward = runMixedRadix(realIn: input.re, imagIn: input.im, inverse: false)
      let backed = runMixedRadix(realIn: forward.re, imagIn: forward.im, inverse: true)
      let scale = 1.0 / Double(n)
      var maxRoundTripDiff = 0.0
      for i in 0..<n {
        let rDiff = abs(backed.re[i] * scale - input.re[i])
        let iDiff = abs(backed.im[i] * scale - input.im[i])
        if rDiff > maxRoundTripDiff { maxRoundTripDiff = rDiff }
        if iDiff > maxRoundTripDiff { maxRoundTripDiff = iDiff }
      }
      // FFT round-trip is bounded by ~N · ε. For N = 1120, ε ≈ 2.2e-16,
      // so 5e-13 is generous and not flaky.
      XCTAssertLessThan(
        maxRoundTripDiff, 5e-13,
        "[round-trip N=\(n)] max |back/N - in| = \(maxRoundTripDiff)")
    }
  }

  /// Inverse direction matches the direct inverse DFT.
  func testInverseDirection() throws {
    for n in [5, 7, 21, 49, 1029] {
      let input = randomComplex(n: n, seed: UInt64(n) &+ 1)
      let mr = runMixedRadix(realIn: input.re, imagIn: input.im, inverse: true)
      let direct = directDFT(realIn: input.re, imagIn: input.im, inverse: true)
      let diff = maxAbsDiff(mr, direct)
      let tol = 1e-10 * Double(n)
      XCTAssertLessThan(
        diff, tol, "[inverse N=\(n)] mixed-radix vs direct max |Δ| = \(diff)")
    }
  }

  /// `init?` should return `nil` for sizes whose factorisation includes
  /// any prime > 7. The caller (BluesteinRealFFT) relies on this to fall
  /// back to Bluestein.
  func testUnsupportedFactorsReturnNil() {
    for n in [11, 13, 17, 22, 33, 121] {
      XCTAssertNil(
        MixedRadixFFT(n: n),
        "MixedRadixFFT(n: \(n)) should return nil for prime factor > 7")
    }
  }

  // MARK: - Single test driver

  /// Common driver: run a fixed-seed pseudo-random complex input through
  /// both `MixedRadixFFT` and the direct O(N²) DFT, then compare. The
  /// tolerance scales with N (FFT error grows ~ N·ε).
  private func assertMatchesDirectDFT(n: Int, label: String) throws {
    let input = randomComplex(n: n, seed: UInt64(n))
    let mr = runMixedRadix(realIn: input.re, imagIn: input.im, inverse: false)
    let direct = directDFT(realIn: input.re, imagIn: input.im, inverse: false)
    let diff = maxAbsDiff(mr, direct)
    // Per-bin error scales like `N · ε`; we use `1e-10 · N` to give plenty
    // of headroom while still catching real bugs.
    let tol = 1e-10 * Double(n)
    XCTAssertLessThan(diff, tol, "[\(label)] max |Δ| = \(diff), tol = \(tol)")
  }
}

/// Minimal SplitMix64 PRNG so tests stay deterministic without dragging in
/// a full RNG dependency. Returns IEEE 754 doubles in [0, 1).
private struct SplitMix64 {
  private var state: UInt64
  init(seed: UInt64) { self.state = seed }
  mutating func next() -> UInt64 {
    state = state &+ 0x9E37_79B9_7F4A_7C15
    var z = state
    z = (z ^ (z >> 30)) &* 0xBF58_476D_1CE4_E5B9
    z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
    return z ^ (z >> 31)
  }
  mutating func nextUnit() -> Double {
    // Top 53 bits → [0, 1).
    Double(next() >> 11) * (1.0 / 9_007_199_254_740_992.0)
  }
}
