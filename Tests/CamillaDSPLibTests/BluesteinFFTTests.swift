// Unit tests for `BluesteinFFT`. `MixedRadixFFT` covers every length whose
// prime factors are ≤ 7, so the production resampler never falls back to
// `BluesteinFFT` for the rate ratios in the matrix. These tests exercise it
// directly so a regression in the chirp-z fallback can't slip in
// unnoticed — they verify the algorithm against a direct O(N²) DFT for
// sizes covering both supported and "exotic" prime factorisations.

import Foundation
import XCTest

@testable import CamillaDSPLib

final class BluesteinFFTTests: XCTestCase {

  // MARK: - Reference DFT

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

  private func runBluestein(
    realIn: [Double], imagIn: [Double], inverse: Bool
  ) -> (re: [Double], im: [Double]) {
    let n = realIn.count
    let fft = BluesteinFFT(n: n)
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

  private func maxAbsDiff(
    _ a: (re: [Double], im: [Double]),
    _ b: (re: [Double], im: [Double])
  ) -> Double {
    var maxDiff = 0.0
    for i in 0..<a.re.count {
      maxDiff = max(maxDiff, abs(a.re[i] - b.re[i]))
      maxDiff = max(maxDiff, abs(a.im[i] - b.im[i]))
    }
    return maxDiff
  }

  private func randomComplex(n: Int, seed: UInt64) -> (re: [Double], im: [Double]) {
    var rng = SimpleSplitMix(seed: seed)
    var re = [Double](repeating: 0, count: n)
    var im = [Double](repeating: 0, count: n)
    for i in 0..<n {
      re[i] = rng.nextUnit() * 2.0 - 1.0
      im[i] = rng.nextUnit() * 2.0 - 1.0
    }
    return (re, im)
  }

  // MARK: - Tests

  /// Sizes that prove Bluestein works regardless of factorisation, including
  /// primes > 7 (the cases where MixedRadixFFT's `init?` returns nil).
  func testForwardMatchesDirectDFT() throws {
    // Mix small primes (covered by MR too — ensures equivalence) with
    // primes the MR path can't handle: 11, 13, 17, 19, 23, 29.
    for n in [3, 7, 11, 13, 17, 19, 23, 29, 121, 169] {
      let input = randomComplex(n: n, seed: UInt64(n) &* 0x9E37_79B9)
      let bs = runBluestein(realIn: input.re, imagIn: input.im, inverse: false)
      let direct = directDFT(realIn: input.re, imagIn: input.im, inverse: false)
      let diff = maxAbsDiff(bs, direct)
      // Bluestein involves three inner FFTs at size m ≥ 2N-1, so per-bin
      // error scales like ~m·ε. 1e-9 · N is generous and stable.
      let tol = 1e-9 * Double(n)
      XCTAssertLessThan(
        diff, tol, "[forward N=\(n)] Bluestein vs direct max |Δ| = \(diff)")
    }
  }

  func testInverseMatchesDirectDFT() throws {
    for n in [11, 13, 17, 23, 121] {
      let input = randomComplex(n: n, seed: UInt64(n))
      let bs = runBluestein(realIn: input.re, imagIn: input.im, inverse: true)
      let direct = directDFT(realIn: input.re, imagIn: input.im, inverse: true)
      let diff = maxAbsDiff(bs, direct)
      let tol = 1e-9 * Double(n)
      XCTAssertLessThan(
        diff, tol, "[inverse N=\(n)] Bluestein vs direct max |Δ| = \(diff)")
    }
  }

  /// Forward + inverse → input · N (matches realfft's unnormalised pair).
  func testRoundTrip() throws {
    for n in [11, 13, 17, 22, 121, 169] {
      let input = randomComplex(n: n, seed: UInt64(n) &+ 7)
      let fwd = runBluestein(realIn: input.re, imagIn: input.im, inverse: false)
      let backed = runBluestein(realIn: fwd.re, imagIn: fwd.im, inverse: true)
      let scale = 1.0 / Double(n)
      var maxDiff = 0.0
      for i in 0..<n {
        maxDiff = max(maxDiff, abs(backed.re[i] * scale - input.re[i]))
        maxDiff = max(maxDiff, abs(backed.im[i] * scale - input.im[i]))
      }
      XCTAssertLessThan(
        maxDiff, 5e-13, "[round-trip N=\(n)] max |back/N - in| = \(maxDiff)")
    }
  }

  /// The production resampler exercises this: when `MixedRadixFFT(n:)`
  /// returns nil, `BluesteinRealFFT.init` falls back to `BluesteinFFT`.
  /// Verify a real-FFT length with a prime > 7 in the inner FFT works.
  func testBluesteinRealFFTFallbackForPrimeFactors() throws {
    // length = 22 → halfN = 11, prime → forces Bluestein fallback.
    let length = 22
    let realFFT = BluesteinRealFFT(length: length)
    XCTAssertEqual(realFFT.spectrumLength, length / 2 + 1)

    // Generate a real impulse and confirm the spectrum is a flat 1.0 across
    // all unique bins (DC and Nyquist are real, all others have unit
    // magnitude).
    var input = [Double](repeating: 0, count: length)
    input[0] = 1.0
    var specRe = [Double](repeating: 0, count: realFFT.spectrumLength)
    var specIm = [Double](repeating: 0, count: realFFT.spectrumLength)
    input.withUnsafeBufferPointer { inBuf in
      specRe.withUnsafeMutableBufferPointer { rBuf in
        specIm.withUnsafeMutableBufferPointer { iBuf in
          realFFT.forward(
            realIn: inBuf.baseAddress!,
            specRe: rBuf.baseAddress!,
            specIm: iBuf.baseAddress!)
        }
      }
    }
    for k in 0..<realFFT.spectrumLength {
      let mag = sqrt(specRe[k] * specRe[k] + specIm[k] * specIm[k])
      XCTAssertEqual(
        mag, 1.0, accuracy: 1e-12,
        "[Bluestein-fallback length=22 bin=\(k)] |X| = \(mag)")
    }

    // Inverse round-trip: forward(impulse) → inverse → ~ length · impulse.
    var recovered = [Double](repeating: 0, count: length)
    specRe.withUnsafeBufferPointer { rBuf in
      specIm.withUnsafeBufferPointer { iBuf in
        recovered.withUnsafeMutableBufferPointer { outBuf in
          realFFT.inverse(
            specRe: rBuf.baseAddress!,
            specIm: iBuf.baseAddress!,
            realOut: outBuf.baseAddress!)
        }
      }
    }
    XCTAssertEqual(recovered[0], Double(length), accuracy: 1e-10)
    for i in 1..<length {
      XCTAssertEqual(recovered[i], 0.0, accuracy: 1e-10)
    }
  }
}

/// Local SplitMix64 (kept private to avoid colliding with the one in
/// MixedRadixFFTTests).
private struct SimpleSplitMix {
  private var state: UInt64
  init(seed: UInt64) { state = seed }
  mutating func next() -> UInt64 {
    state = state &+ 0x9E37_79B9_7F4A_7C15
    var z = state
    z = (z ^ (z >> 30)) &* 0xBF58_476D_1CE4_E5B9
    z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
    return z ^ (z >> 31)
  }
  mutating func nextUnit() -> Double {
    Double(next() >> 11) * (1.0 / 9_007_199_254_740_992.0)
  }
}
