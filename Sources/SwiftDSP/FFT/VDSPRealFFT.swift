// vDSP `fft_zrip` backend for power-of-two real-FFT lengths.
//
// Selected by `RealFFT.init` when `length` is a power of two
// `≥ 8`. vDSP's hand-tuned NEON/SSE radix-2 split-complex real FFT is
// the fastest path on Apple Silicon — for our resampler matrix it
// roughly doubles the throughput of the "complex-FFT-via-half-N" path
// for sizes like 1024/2048/4096.

import Accelerate
import Foundation

/// Wraps Apple's `vDSP_fft_zrip` (radix-2 split-complex real FFT). vDSP's
/// internal scaling is asymmetric — forward applies a `2×` factor, inverse
/// does not — so we fold a `0.5` factor into the spectrum unpack on the
/// forward path. The externally observed semantics then match
/// `ComplexInnerRealFFT` exactly: forward = unscaled DFT, inverse =
/// `length · signal`.
///
/// vDSP's spectrum packing: DC is in `realp[0]`, Nyquist in `imagp[0]`,
/// bins `1..N-1` in `realp[k] + i·imagp[k]`. Our public API exposes the
/// `N+1` unique bins as flat `specRe`/`specIm` arrays with DC at index 0,
/// Nyquist at index N — this backend repacks accordingly.
final class VDSPRealFFT<T: FFTRealPrecision>: RealFFTBackend {
  typealias Element = T

  private let halfN: Int
  private let log2n: vDSP_Length
  private let setup: T.SetupType
  // Split-complex scratch of length halfN (= length/2).
  private let scratchRe: UnsafeMutablePointer<T>
  private let scratchIm: UnsafeMutablePointer<T>

  /// Returns `nil` when `length` is not a power of two `≥ 8`, or when
  /// setup creation fails — caller falls back to the
  /// complex-inner backend.
  init?(length: Int) {
    guard length >= 8, length.nonzeroBitCount == 1 else { return nil }
    let log2nVal = vDSP_Length(length.trailingZeroBitCount)
    guard let setup = T.createSetup(log2n: log2nVal, radix: FFTRadix(kFFTRadix2)) else {
      return nil
    }
    self.halfN = length / 2
    self.log2n = log2nVal
    self.setup = setup
    self.scratchRe = .allocate(capacity: halfN)
    self.scratchIm = .allocate(capacity: halfN)
  }

  deinit {
    T.destroySetup(setup)
    scratchRe.deallocate()
    scratchIm.deallocate()
  }

  func forward(
    realIn: UnsafePointer<T>,
    specRe: UnsafeMutablePointer<T>,
    specIm: UnsafeMutablePointer<T>
  ) {
    let n = halfN
    // Deinterleave 2N real samples into N split-complex pairs:
    // scratch.real[k] = realIn[2k], scratch.imag[k] = realIn[2k+1].
    var split = T.initSplitComplex(realp: scratchRe, imagp: scratchIm)
    realIn.withMemoryRebound(to: T.ComplexType.self, capacity: n) { complexIn in
      T.ctoz(complexIn, 2, &split, 1, vDSP_Length(n))
    }
    // In-place real-to-complex forward FFT. vDSP scales by 2.
    T.fft_zrip(setup, &split, 1, log2n, FFTDirection(kFFTDirection_Forward))

    // Repack vDSP's packed spectrum into our flat (N+1)-bin layout, folding
    // the `0.5` un-scale into the copy. After:
    //   specRe[0]   = vDSP_DC / 2 = unscaled DC
    //   specIm[0]   = 0
    //   specRe[k]   = vDSP_Re[k] / 2   for k = 1..N-1
    //   specIm[k]   = vDSP_Im[k] / 2   for k = 1..N-1
    //   specRe[N]   = vDSP_Im[0] / 2   (Nyquist was packed in imagp[0])
    //   specIm[N]   = 0
    var half: T = 0.5
    T.vsmul(scratchRe, 1, &half, specRe, 1, vDSP_Length(n))
    if n > 1 {
      T.vsmul(scratchIm + 1, 1, &half, specIm + 1, 1, vDSP_Length(n - 1))
    }
    specIm[0] = 0
    specRe[n] = scratchIm[0] * 0.5
    specIm[n] = 0
  }

  func inverse(
    specRe: UnsafePointer<T>,
    specIm: UnsafePointer<T>,
    realOut: UnsafeMutablePointer<T>
  ) {
    let n = halfN
    // Repack our flat (N+1)-bin layout back into vDSP's packed format
    // (DC in realp[0], Nyquist in imagp[0], bins 1..N-1 in realp[k]/imagp[k]).
    scratchRe[0] = specRe[0]
    scratchIm[0] = specRe[n]
    if n > 1 {
      (scratchRe + 1).update(from: specRe + 1, count: n - 1)
      (scratchIm + 1).update(from: specIm + 1, count: n - 1)
    }

    var split = T.initSplitComplex(realp: scratchRe, imagp: scratchIm)
    T.fft_zrip(setup, &split, 1, log2n, FFTDirection(kFFTDirection_Inverse))

    // Asymmetric vDSP scaling: forward applies a `2×` factor, inverse
    // does not. Feeding unscaled bins (we already halved the forward
    // output) directly produces the unnormalised IDFT result —
    // `length · signal` — which is exactly the RealFFT
    // convention. No extra scaling needed here.
    //
    // Re-interleave split-complex back to 2N reals: realOut[2k] = split.real[k],
    // realOut[2k+1] = split.imag[k].
    realOut.withMemoryRebound(to: T.ComplexType.self, capacity: n) { complexOut in
      T.ztoc(&split, 1, complexOut, 2, vDSP_Length(n))
    }
  }
}
