// Unit tests for `WindowFunction.swift`. The production code uses
// only the squared 4-term Blackman-Harris window; these tests verify
// it via structural properties from window-function theory rather
// than by comparing numerical values to a specific implementation.

import Foundation
import Testing

@testable import DSPAudio
@testable import DSPFFT

@Suite struct WindowFunctionTests {

  // MARK: - blackmanHarris2Value

  /// At `i = N/2` (the centre of an even-length periodic window), the
  /// 4-term Blackman-Harris coefficients sum to 1.0 — the cosines
  /// evaluate to ±1 at integer multiples of π, which is what `i = N/2`
  /// gives. Squaring then yields 1.0.
  @Test func CentrePeakIsOne() {
    for n in [32, 64, 128, 256, 1024] {
      let v = blackmanHarris2Value(i: n / 2, n: n)
      #expect(abs(v - 1.0) <= 1e-12, "centre peak at n=\(n) was \(v)")
    }
  }

  /// At `i = 0` the cosines all evaluate to 1, so the window value
  /// is the algebraic sum of the (signed) coefficients squared.
  /// Harris's coefficients sum to a tiny non-zero residual — this
  /// test pins down that we're using the published values.
  @Test func EndpointMatchesCoefficientSum() {
    let n = 64
    // a₀ − a₁ + a₂ − a₃ from Harris (1978) Table 6, "-92 dB" row.
    let bhAtZero = 0.35875 - 0.48829 + 0.14128 - 0.01168
    let expected = bhAtZero * bhAtZero
    let v = blackmanHarris2Value(i: 0, n: n)
    #expect(abs(v - expected) <= 1e-15)
  }

  /// Periodic by construction: the divisor is `n`, not `n − 1`, so
  /// `w(i) == w(i + n)` exactly.
  @Test func IsPeriodic() {
    let n = 32
    for i in 0..<n {
      let v0 = blackmanHarris2Value(i: i, n: n)
      let v1 = blackmanHarris2Value(i: i + n, n: n)
      #expect(abs(v0 - v1) <= 1e-13, "periodicity broken at i=\(i)")
    }
  }

  /// Symmetric around `n/2`: `w(i) == w(n − i)` for the periodic
  /// 4-term cosine window.
  @Test func IsSymmetricAroundCentre() {
    let n = 64
    for i in 1..<(n / 2) {
      let left = blackmanHarris2Value(i: i, n: n)
      let right = blackmanHarris2Value(i: n - i, n: n)
      #expect(abs(left - right) <= 1e-13)
    }
  }

  /// Non-negative everywhere — squaring guarantees `w² ≥ 0`.
  @Test func IsNonNegative() {
    let n = 128
    for i in 0..<n {
      let v = blackmanHarris2Value(i: i, n: n)
      #expect(v >= 0, "negative value \(v) at i=\(i)")
    }
  }

  // MARK: - cutoffForBlackmanHarris2

  /// Cutoff stays inside `(0, 1)` for every length — `0` would mean
  /// no passband, `1` would alias the main lobe past Nyquist.
  @Test func CutoffStaysInsideUnitInterval() {
    for n in [32, 64, 128, 256, 1024, 4096] {
      let c = cutoffForBlackmanHarris2(filterLength: n)
      #expect(c > 0 && c < 1, "cutoff(\(n)) = \(c) is out of range")
    }
  }

  /// Longer filters get a wider passband — the transition margin
  /// `~16/N` shrinks with `N`, so cutoff increases monotonically.
  @Test func CutoffIsMonotonicInLength() {
    let lens = [32, 64, 128, 256, 1024, 4096]
    var prev = 0.0
    for n in lens {
      let c = cutoffForBlackmanHarris2(filterLength: n)
      #expect(c > prev, "cutoff(\(n))=\(c) not greater than previous \(prev)")
      prev = c
    }
  }

  /// Cutoff approaches 1.0 as `N → ∞` — both the `1/N` and `1/N²`
  /// correction terms vanish. At `N = 1_048_576` the residual is on
  /// the order of `16/N ≈ 1.5e-5`, well inside the bound below.
  @Test func CutoffConvergesToOne() {
    let large = cutoffForBlackmanHarris2(filterLength: 1_048_576)
    #expect(large > 0.9999 && large < 1.0, "long-N cutoff \(large) didn't approach 1.0")
  }

  // MARK: - makeBlackmanHarris2SincKernel

  /// `Σh = 1` (unity DC gain) — that's what the normalisation step
  /// guarantees, and it's what the resampler depends on for
  /// gain-correct output.
  @Test func KernelHasUnityDCGain() {
    for n in [32, 64, 128, 256, 1024] {
      let cutoff = cutoffForBlackmanHarris2(filterLength: n)
      let kernel = makeBlackmanHarris2SincKernel(length: n, cutoff: cutoff)
      #expect(kernel.count == n)
      let sum = kernel.reduce(0.0, +)
      #expect(abs(sum - 1.0) <= 1e-12, "n=\(n) sum=\(sum)")
    }
  }

  /// The kernel is symmetric around its centre — windowed-sinc with a
  /// symmetric window stays symmetric, which gives the FIR filter a
  /// linear phase response.
  @Test func KernelIsSymmetric() {
    let n = 128
    let cutoff = cutoffForBlackmanHarris2(filterLength: n)
    let kernel = makeBlackmanHarris2SincKernel(length: n, cutoff: cutoff)
    // Even N: there are N/2 mirror pairs; the lone unpaired tap is at
    // index N/2 (the centre). For a periodic window of length N, the
    // implicit "would-be index N" sample equals index 0, so the
    // symmetry pair for index 0 is simply itself.
    for i in 1..<(n / 2) {
      #expect(
        abs(kernel[i] - kernel[n - i]) <= 1e-13,
        "asymmetry at i=\(i): \(kernel[i]) vs \(kernel[n - i])")
    }
  }

  /// The centre tap is the largest in magnitude — sinc(0) · w²(N/2) =
  /// 1 · 1 (before normalisation), and normalisation just rescales.
  @Test func KernelPeaksAtCentre() {
    let n = 128
    let cutoff = cutoffForBlackmanHarris2(filterLength: n)
    let kernel = makeBlackmanHarris2SincKernel(length: n, cutoff: cutoff)
    let centre = kernel[n / 2]
    for i in 0..<n where i != n / 2 {
      #expect(
        abs(kernel[i]) <= abs(centre),
        "tap \(i) (\(kernel[i])) larger than centre (\(centre))")
    }
  }
}
