// Real-FFT backend that builds a 2N-point real FFT from one N-point
// complex FFT plus an O(N) "untwiddle" pass. Used for any even length
// that doesn't qualify for `VDSPRealFFT` (i.e. non-power-of-two, or
// pow2 < 8).
//
// The inner N-point complex FFT is supplied by the caller —
// `RealFFT.init` picks between `VDSPComplexDFT`,
// `MixedRadixFFT`, and `BluesteinFFT` based on `halfN`'s factorisation.
// This class stays purely about the real-FFT structure (packing,
// untwiddle, inverse unpack) and never re-decides the backend.
//
// Algorithm references:
//   - https://www.dsprelated.com/showarticle/4.php (Real FFT from complex FFT)
//   - https://en.wikipedia.org/wiki/Fast_Fourier_transform#Real-input_FFTs

import Accelerate
import Foundation

/// Computes a 2N-point real FFT via an N-point complex FFT plus an O(N)
/// untwiddle. The inner complex FFT is supplied by the caller —
/// `RealFFT.init` does the priority-based selection so this
/// class stays purely about the real-FFT structure (packing, untwiddle,
/// inverse unpack) and never re-decides the backend.
final class ComplexInnerRealFFT: RealFFTBackend {
  private let halfN: Int  // = length / 2 = N
  /// The N-point complex FFT picked at construction. Could be any
  /// `ArbitraryComplexFFT` — `VDSPComplexDFT`, `MixedRadixFFT`, or
  /// `BluesteinFFT` depending on what `RealFFT.init` chose.
  private let inner: ArbitraryComplexFFT

  // Unit-modulus twiddle table `W[k] = exp(-iπk/N)` for k = 0..N-1.
  private let twiddleRe: UnsafeMutablePointer<Double>
  private let twiddleIm: UnsafeMutablePointer<Double>

  // Hot-path scratch (length N).
  private let zRe: UnsafeMutablePointer<Double>
  private let zIm: UnsafeMutablePointer<Double>
  private let zFRe: UnsafeMutablePointer<Double>
  private let zFIm: UnsafeMutablePointer<Double>

  init(length: Int, inner: ArbitraryComplexFFT) {
    self.halfN = length / 2
    self.inner = inner

    self.twiddleRe = .allocate(capacity: halfN)
    self.twiddleIm = .allocate(capacity: halfN)
    for k in 0..<halfN {
      let theta = -.pi * Double(k) / Double(halfN)
      twiddleRe[k] = cos(theta)
      twiddleIm[k] = sin(theta)
    }

    self.zRe = .allocate(capacity: halfN)
    self.zIm = .allocate(capacity: halfN)
    self.zFRe = .allocate(capacity: halfN)
    self.zFIm = .allocate(capacity: halfN)
  }

  deinit {
    twiddleRe.deallocate()
    twiddleIm.deallocate()
    zRe.deallocate()
    zIm.deallocate()
    zFRe.deallocate()
    zFIm.deallocate()
  }

  func forward(
    realIn: UnsafePointer<Double>,
    specRe: UnsafeMutablePointer<Double>,
    specIm: UnsafeMutablePointer<Double>
  ) {
    let n = halfN

    // Pack the 2N real samples into N complex: z[k] = x[2k] + i·x[2k+1].
    // Reinterpret `realIn` as interleaved complex pairs and let `vDSP_ctozD`
    // do the deinterleave in one pass.
    var zSplit = DSPDoubleSplitComplex(realp: zRe, imagp: zIm)
    realIn.withMemoryRebound(to: DSPDoubleComplex.self, capacity: n) { complexIn in
      vDSP_ctozD(complexIn, 2, &zSplit, 1, vDSP_Length(n))
    }

    // Z = FFT_N(z). Unnormalised forward.
    inner.execute(
      realIn: zRe, imagIn: zIm, realOut: zFRe, imagOut: zFIm, inverse: false)

    // DC and Nyquist bins (both real):
    //   X[0] = Re(Z[0]) + Im(Z[0])
    //   X[N] = Re(Z[0]) - Im(Z[0])
    let z0r = zFRe[0]
    let z0i = zFIm[0]
    specRe[0] = z0r + z0i
    specIm[0] = 0
    specRe[n] = z0r - z0i
    specIm[n] = 0

    // Generic untwiddle for k ∈ [1, N):
    //   E[k] = ½ · (Z[k] + conj(Z[N-k]))
    //   O[k] = -½·i · (Z[k] - conj(Z[N-k]))
    //   X[k] = E[k] + W^k · O[k],  W^k = exp(-iπk/N)
    //
    // SIMD2 path processes consecutive `k` pairs. The partners (N-k, N-k-1)
    // are also adjacent in memory but in reversed order — we build the
    // SIMD2 explicitly to keep lane 0 = `k` and lane 1 = `k+1`.
    let pairEnd = ((n - 1) & ~1) + 1  // last odd k handled by SIMD2 = pairEnd - 1
    var k = 1
    while k < pairEnd {
      let zkR = ldSIMD2(zFRe, k)
      let zkI = ldSIMD2(zFIm, k)
      let zmR = SIMD2<Double>(zFRe[n - k], zFRe[n - k - 1])
      let zmI = SIMD2<Double>(zFIm[n - k], zFIm[n - k - 1])
      let eRe = 0.5 * (zkR + zmR)
      let eIm = 0.5 * (zkI - zmI)
      let diffRe = zkR - zmR
      let diffIm = zkI + zmI
      let oRe = 0.5 * diffIm
      let oIm = -0.5 * diffRe
      let twR = ldSIMD2(twiddleRe, k)
      let twI = ldSIMD2(twiddleIm, k)
      let woRe = twR * oRe - twI * oIm
      let woIm = twR * oIm + twI * oRe
      let outRe = eRe + woRe
      let outIm = eIm + woIm
      stSIMD2(specRe, k, outRe)
      stSIMD2(specIm, k, outIm)
      k += 2
    }
    while k < n {
      let zkR = zFRe[k]
      let zkI = zFIm[k]
      let zmR = zFRe[n - k]
      let zmI = zFIm[n - k]
      let eRe = 0.5 * (zkR + zmR)
      let eIm = 0.5 * (zkI - zmI)
      let diffRe = zkR - zmR
      let diffIm = zkI + zmI
      let oRe = 0.5 * diffIm
      let oIm = -0.5 * diffRe
      let twR = twiddleRe[k]
      let twI = twiddleIm[k]
      let woRe = twR * oRe - twI * oIm
      let woIm = twR * oIm + twI * oRe
      specRe[k] = eRe + woRe
      specIm[k] = eIm + woIm
      k += 1
    }
  }

  func inverse(
    specRe: UnsafePointer<Double>,
    specIm: UnsafePointer<Double>,
    realOut: UnsafeMutablePointer<Double>
  ) {
    let n = halfN

    // DC bin packs the special pair (X[0], X[N]):
    //   z[0] = ½·(X[0] + X[N]) + ½·i·(X[0] - X[N])
    let x0 = specRe[0]
    let xN = specRe[n]
    zRe[0] = 0.5 * (x0 + xN)
    zIm[0] = 0.5 * (x0 - xN)

    // Generic inverse untwiddle for k ∈ [1, N):
    //   E[k] = ½·(X[k] + conj(X[N-k]))
    //   O[k] = ½·conj(W^k)·(X[k] - conj(X[N-k]))
    //   z[k] = E[k] + i·O[k]
    //
    // SIMD2 path: same partner-mirror trick as in `forward()`.
    let pairEnd = ((n - 1) & ~1) + 1
    var k = 1
    while k < pairEnd {
      let xkR = ldSIMD2(specRe, k)
      let xkI = ldSIMD2(specIm, k)
      let xmR = SIMD2<Double>(specRe[n - k], specRe[n - k - 1])
      let xmI = SIMD2<Double>(specIm[n - k], specIm[n - k - 1])
      let eRe = 0.5 * (xkR + xmR)
      let eIm = 0.5 * (xkI - xmI)
      let halfDiffRe = 0.5 * (xkR - xmR)
      let halfDiffIm = 0.5 * (xkI + xmI)
      let twR = ldSIMD2(twiddleRe, k)
      let twI = ldSIMD2(twiddleIm, k)
      let oRe = halfDiffRe * twR + halfDiffIm * twI
      let oIm = halfDiffIm * twR - halfDiffRe * twI
      let zR = eRe - oIm
      let zI = eIm + oRe
      stSIMD2(zRe, k, zR)
      stSIMD2(zIm, k, zI)
      k += 2
    }
    while k < n {
      let xkR = specRe[k]
      let xkI = specIm[k]
      let xmR = specRe[n - k]
      let xmI = specIm[n - k]
      let eRe = 0.5 * (xkR + xmR)
      let eIm = 0.5 * (xkI - xmI)
      let halfDiffRe = 0.5 * (xkR - xmR)
      let halfDiffIm = 0.5 * (xkI + xmI)
      let twR = twiddleRe[k]
      let twI = twiddleIm[k]
      let oRe = halfDiffRe * twR + halfDiffIm * twI
      let oIm = halfDiffIm * twR - halfDiffRe * twI
      zRe[k] = eRe - oIm
      zIm[k] = eIm + oRe
      k += 1
    }

    // Inner inverse FFT. The inner returns the unnormalised N-point IFFT,
    // i.e. `N · z`. The textbook unnormalised 2N-point IFFT equals `2 · N · z`,
    // so the unpack picks up a factor of 2.
    inner.execute(
      realIn: zRe, imagIn: zIm, realOut: zFRe, imagOut: zFIm, inverse: true)

    // Scale by 2 in place, then re-interleave back into `realOut` via
    // `vDSP_ztocD`. Two vDSP calls beat the scalar 2N store loop on Apple
    // Silicon when n ≥ ~1k.
    var two = 2.0
    vDSP_vsmulD(zFRe, 1, &two, zFRe, 1, vDSP_Length(n))
    vDSP_vsmulD(zFIm, 1, &two, zFIm, 1, vDSP_Length(n))
    var zFSplit = DSPDoubleSplitComplex(realp: zFRe, imagp: zFIm)
    realOut.withMemoryRebound(to: DSPDoubleComplex.self, capacity: n) { complexOut in
      vDSP_ztocD(&zFSplit, 1, complexOut, 2, vDSP_Length(n))
    }
  }
}
