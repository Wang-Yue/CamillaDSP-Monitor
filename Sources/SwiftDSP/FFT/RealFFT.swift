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
//      `vDSP_fft_zrip` / `vDSP_fft_zripD`.
//   2. Otherwise (arbitrary even length): a 2N-point real FFT is built
//      from one N-point complex FFT plus an O(N) untwiddle pass —
//      `ComplexInnerRealFFT` (`ComplexInnerRealFFT.swift`). (Only supported for Double).
//      The inner complex FFT is itself routed here, in priority order:
//      a. `VDSPComplexDFT` (`VDSPComplexDFT.swift`) — `vDSP_DFT_zopD`
//         for sizes `f·2ᵐ`, `f ∈ {1, 3, 5, 15}`, `m ≥ 3`.
//      b. `MixedRadixFFT` (`MixedRadixFFT.swift`) — native mixed-radix
//         for prime factorisations in `{2, 3, 5, 7}`. Its radix-2/4/8
//         stages handle the *power-of-two portion* of a mixed factorisation
//         (e.g. `1120 = 2⁵·5·7` factored as `[8, 4, 5, 7]`).
//      c. `BluesteinFFT` (`BluesteinFFT.swift`) — universal fallback
//         for anything with a prime factor `> 7` (e.g. halfN = 1034 has primes 11 and 47).
//
// Every backend exposes the same external semantics — forward =
// unscaled DFT, inverse = `length · signal` — so the resampler is
// oblivious to which path runs.
//
// Algorithm references:
//   - https://www.dsprelated.com/showarticle/4.php (Real FFT from complex FFT)
//   - https://en.wikipedia.org/wiki/Fast_Fourier_transform#Real-input_FFTs

import Foundation

/// Module-internal protocol implemented by every real-FFT backend.
public protocol RealFFTBackend<Element>: AnyObject {
  associatedtype Element: FFTRealPrecision

  func forward(
    realIn: UnsafePointer<Element>,
    specRe: UnsafeMutablePointer<Element>,
    specIm: UnsafeMutablePointer<Element>)
  func inverse(
    specRe: UnsafePointer<Element>,
    specIm: UnsafePointer<Element>,
    realOut: UnsafeMutablePointer<Element>)
}

public typealias RealFFT = GenericRealFFT<Double>

public enum RealFFTError: Error, CustomStringConvertible {
  case invalidLength(String)
  case unsupportedPrecision(String)
  case setupFailed(String)

  public var description: String {
    switch self {
    case .invalidLength(let msg): return "RealFFT length error: \(msg)"
    case .unsupportedPrecision(let msg): return "RealFFT unsupported precision: \(msg)"
    case .setupFailed(let msg): return "RealFFT setup failed: \(msg)"
    }
  }
}

/// Real-input/output FFT of length `length = 2N` (even). Forward
/// produces the `N + 1` unique complex bins; inverse consumes them.
/// Caller is responsible for any `1/length` normalisation.
public final class GenericRealFFT<T: FFTRealPrecision> {
  /// Time-domain length (must be even).
  public let length: Int

  /// Number of unique complex bins in the spectrum (= length/2 + 1).
  public var spectrumLength: Int { length / 2 + 1 }

  private let backend: any RealFFTBackend<T>

  public init(length: Int) throws {
    guard length > 0 else {
      throw RealFFTError.invalidLength("RealFFT: length must be positive, got \(length)")
    }
    guard length % 2 == 0 else {
      throw RealFFTError.invalidLength("RealFFT: length must be even, got \(length)")
    }
    self.length = length

    // Branch 1: power-of-2 → vDSP's tuned real FFT, no complex-inner
    // detour. `length >= 8` is the smallest size `vDSP_fft_zrip`
    // supports; smaller pow2 lengths fall through to branch 2.
    if let vdsp = VDSPRealFFT<T>(length: length) {
      self.backend = vdsp
      return
    }

    // Branch 2: even but not power-of-2 (or pow2 < 8). Currently fallback paths
    // are only compiled/supported for Double precision.
    if T.self == Double.self {
      let halfN = length / 2
      let inner: ArbitraryComplexFFT
      if let dft = VDSPComplexDFT(n: halfN) {
        inner = dft
      } else if let mr = MixedRadixFFT(n: halfN) {
        inner = mr
      } else {
        inner = try BluesteinFFT(n: halfN)
      }
      let doubleBackend = ComplexInnerRealFFT(length: length, inner: inner)
      self.backend = doubleBackend as! any RealFFTBackend<T>
    } else {
      throw RealFFTError.unsupportedPrecision("RealFFT for Float only supports power-of-two sizes >= 8")
    }
  }

  /// Forward 2N-point real FFT. Produces the `N + 1` unique complex bins.
  /// `realIn` length must be ≥ `length`; `specRe`/`specIm` length must be
  /// ≥ `spectrumLength`.
  @inline(__always)
  public func forward(
    realIn: UnsafePointer<T>,
    specRe: UnsafeMutablePointer<T>,
    specIm: UnsafeMutablePointer<T>
  ) {
    backend.forward(realIn: realIn, specRe: specRe, specIm: specIm)
  }

  /// Inverse 2N-point real FFT. Reads the `N + 1` unique complex bins from
  /// `specRe`/`specIm` and writes `length` real samples into `realOut`.
  /// Output is scaled by `length` (round-trip with `forward` multiplies by
  /// `length`).
  @inline(__always)
  public func inverse(
    specRe: UnsafePointer<T>,
    specIm: UnsafePointer<T>,
    realOut: UnsafeMutablePointer<T>
  ) {
    backend.inverse(specRe: specRe, specIm: specIm, realOut: realOut)
  }
}
