// Real-input FFT of arbitrary even length. `RealFFT.init` is
// the **single dispatch point** for the resampler's FFT subsystem — it
// inspects the requested length once and picks the fastest available
// backend, so callers (and the per-backend classes) never repeat that
// decision.
//
// Decision tree (top-to-bottom, first match wins)
// ------------------------------------------------
//   1. `length` is a power of two `≥ 8`
//      → `VDSPRealFFT` (`VDSPRealFFT.swift`), wrapping Apple's
//      `vDSP_fft_zripD` (radix-2 split-complex real FFT, hand-tuned
//      NEON on Apple Silicon).
//   2. Otherwise (arbitrary even length): a 2N-point real FFT is built
//      from one N-point complex FFT plus an O(N) untwiddle pass —
//      `ComplexInnerRealFFT` (`ComplexInnerRealFFT.swift`). The inner
//      complex FFT is itself routed here, in priority order:
//      a. `VDSPComplexDFT` (`VDSPComplexDFT.swift`) — `vDSP_DFT_zopD`
//         for sizes `f·2ᵐ`, `f ∈ {1, 3, 5, 15}`, `m ≥ 3`.
//      b. `MixedRadixFFT` (`MixedRadixFFT.swift`) — native mixed-radix
//         for prime factorisations in `{2, 3, 5, 7}`. Its radix-2/4/8
//         stages are NOT redundant with branch (1): they handle the
//         *power-of-two portion* of a mixed factorisation (e.g.
//         `1120 = 2⁵·5·7` factored as `[8, 4, 5, 7]`). Without them
//         MixedRadix could only support odd-only sizes like
//         `105 = 3·5·7`.
//      c. `BluesteinFFT` (`BluesteinFFT.swift`) — universal fallback
//         for anything with a prime factor `> 7` (e.g. our `11→13k`
//         rate pair, halfN = 1034 has primes 11 and 47).
//
// Every backend exposes the same external semantics — forward =
// unscaled DFT, inverse = `length · signal` — so the resampler is
// oblivious to which path runs.
//
// Algorithm references:
//   - https://www.dsprelated.com/showarticle/4.php (Real FFT from complex FFT)
//   - https://en.wikipedia.org/wiki/Fast_Fourier_transform#Real-input_FFTs

import Foundation

/// Module-internal protocol implemented by every real-FFT backend
/// `RealFFT` can dispatch to. Forward = unscaled DFT, inverse
/// = `length · signal` (round-trip with `forward` multiplies by
/// `length`). The protocol-witness call is paid once per `forward` /
/// `inverse` (twice per resampler chunk per channel) and is invisible
/// against the actual FFT cost.
protocol RealFFTBackend: AnyObject {
  func forward(
    realIn: UnsafePointer<Double>,
    specRe: UnsafeMutablePointer<Double>,
    specIm: UnsafeMutablePointer<Double>)
  func inverse(
    specRe: UnsafePointer<Double>,
    specIm: UnsafePointer<Double>,
    realOut: UnsafeMutablePointer<Double>)
}

/// Real-input/output FFT of length `length = 2N` (even). Forward
/// produces the `N + 1` unique complex bins; inverse consumes them.
/// Caller is responsible for any `1/length` normalisation.
///
/// `init(length:)` is the project's single FFT-backend selector — see
/// the file-level header for the routing decision tree. Callers never
/// see (or pick) a backend; they just get a correctly-sized real FFT.
public final class RealFFT {
  /// Time-domain length (must be even).
  public let length: Int

  /// Number of unique complex bins in the spectrum (= length/2 + 1).
  public var spectrumLength: Int { length / 2 + 1 }

  private let backend: RealFFTBackend

  public init(length: Int) {
    precondition(length > 0, "RealFFT: length must be positive")
    precondition(length % 2 == 0, "RealFFT: length must be even")
    self.length = length

    // Branch 1: power-of-2 → vDSP's tuned real FFT, no complex-inner
    // detour. `length >= 8` is the smallest size `vDSP_fft_zripD`
    // supports; smaller pow2 lengths fall through to branch 2.
    if let vdsp = VDSPRealFFT(length: length) {
      self.backend = vdsp
      return
    }

    // Branch 2: even but not power-of-2 (or pow2 < 8). Build the
    // 2N-point real FFT from an N-point complex FFT. Pick the inner
    // complex FFT once, here, in priority order — `ComplexInnerRealFFT`
    // itself just consumes the chosen `inner`.
    let halfN = length / 2
    let inner: ArbitraryComplexFFT
    if let dft = VDSPComplexDFT(n: halfN) {
      inner = dft
    } else if let mr = MixedRadixFFT(n: halfN) {
      inner = mr
    } else {
      inner = BluesteinFFT(n: halfN)
    }
    self.backend = ComplexInnerRealFFT(length: length, inner: inner)
  }

  /// Forward 2N-point real FFT. Produces the `N + 1` unique complex bins.
  /// `realIn` length must be ≥ `length`; `specRe`/`specIm` length must be
  /// ≥ `spectrumLength`.
  @inline(__always)
  public func forward(
    realIn: UnsafePointer<Double>,
    specRe: UnsafeMutablePointer<Double>,
    specIm: UnsafeMutablePointer<Double>
  ) {
    backend.forward(realIn: realIn, specRe: specRe, specIm: specIm)
  }

  /// Inverse 2N-point real FFT. Reads the `N + 1` unique complex bins from
  /// `specRe`/`specIm` and writes `length` real samples into `realOut`.
  /// Output is scaled by `length` (round-trip with `forward` multiplies by
  /// `length`).
  @inline(__always)
  public func inverse(
    specRe: UnsafePointer<Double>,
    specIm: UnsafePointer<Double>,
    realOut: UnsafeMutablePointer<Double>
  ) {
    backend.inverse(specRe: specRe, specIm: specIm, realOut: realOut)
  }
}
