// Arbitrary-N complex DFT via Bluestein's chirp-z transform.
//
// vDSP's complex FFT (`vDSP_DFT_zop_CreateSetupD`) is restricted to power-of-2
// lengths ≥ 16 — fine for "nice" sizes but unusable for the awkward FFT sizes
// rubato's `Fft` resampler picks (e.g. 2058 for 44.1↔48 kHz). Bluestein turns
// any N into a length-M cyclic convolution where M = next power of 2 ≥ 2N - 1,
// which we then run through vDSP. Cost is ~3 inner FFTs per logical FFT, but
// it's still O(N log N) and matches rubato's `realfft`-backed FFT in
// floating-point semantics.
//
// Storage uses raw `UnsafeMutablePointer<Double>` buffers (allocated in init,
// freed in deinit) so the hot path can hand them straight to vDSP without
// nested `withUnsafe*` closures. All complex multiplications run through
// `vDSP_zvmulD`, which on Apple Silicon issues packed NEON `fmla.2d` pairs.

import Accelerate
import Foundation

/// Computes the unnormalised forward DFT
///   `X[k] = Σₙ x[n] · exp(-2πi · n · k / N)`
/// or the unnormalised inverse DFT
///   `x[n] = Σₖ X[k] · exp(+2πi · n · k / N)`
/// for arbitrary `N > 0`. Inverse callers that want the true inverse must
/// divide by `N` themselves — this matches rubato's `realfft` convention,
/// where the forward and inverse transforms are both scale-free and the
/// resampler relies on the filter prefactor for normalisation.
final class BluesteinFFT {
  /// Logical DFT length.
  let n: Int

  /// Inner power-of-2 FFT length, ≥ 2n − 1 and ≥ 16 (vDSP's minimum).
  private let m: Int

  // Forward chirp `α[k] = exp(-iπk²/N)`, length n. Stored as
  // (cos(πk²/N), -sin(πk²/N)).
  private let alphaRe: UnsafeMutablePointer<Double>
  private let alphaIm: UnsafeMutablePointer<Double>

  // Same chirp pre-scaled by 1/m, used in the post-multiply step. Folding
  // the IFFT's missing 1/m scale into α here lets us skip two
  // length-m `vDSP_vsmulD` calls per execute() call.
  private let alphaPostRe: UnsafeMutablePointer<Double>
  private let alphaPostIm: UnsafeMutablePointer<Double>

  // Pre-FFT'd b sequence (length m), used in the convolution step.
  private let bRealF: UnsafeMutablePointer<Double>
  private let bImagF: UnsafeMutablePointer<Double>

  private let fftFwd: vDSP_DFT_Setup
  private let fftInv: vDSP_DFT_Setup

  // Hot-path scratch (length m).
  private let aRe: UnsafeMutablePointer<Double>
  private let aIm: UnsafeMutablePointer<Double>
  private let aReF: UnsafeMutablePointer<Double>
  private let aImF: UnsafeMutablePointer<Double>
  private let pRe: UnsafeMutablePointer<Double>
  private let pIm: UnsafeMutablePointer<Double>
  private let cRe: UnsafeMutablePointer<Double>
  private let cIm: UnsafeMutablePointer<Double>

  init(n: Int) {
    precondition(n > 0, "BluesteinFFT: n must be positive")
    self.n = n

    var m = 1
    while m < (2 * n - 1) { m <<= 1 }
    m = max(m, 16)
    self.m = m

    guard
      let fwd = vDSP_DFT_zop_CreateSetupD(nil, vDSP_Length(m), .FORWARD),
      let inv = vDSP_DFT_zop_CreateSetupD(nil, vDSP_Length(m), .INVERSE)
    else {
      fatalError("BluesteinFFT: vDSP DFT setup failed for inner size \(m)")
    }
    self.fftFwd = fwd
    self.fftInv = inv

    self.alphaRe = .allocate(capacity: n)
    self.alphaIm = .allocate(capacity: n)
    self.alphaPostRe = .allocate(capacity: n)
    self.alphaPostIm = .allocate(capacity: n)
    self.bRealF = .allocate(capacity: m)
    self.bImagF = .allocate(capacity: m)
    self.aRe = .allocate(capacity: m)
    self.aIm = .allocate(capacity: m)
    self.aReF = .allocate(capacity: m)
    self.aImF = .allocate(capacity: m)
    self.pRe = .allocate(capacity: m)
    self.pIm = .allocate(capacity: m)
    self.cRe = .allocate(capacity: m)
    self.cIm = .allocate(capacity: m)

    // Initialize α[k] = exp(-iπk²/N). The (k*k) % (2*n) reduction keeps the
    // trig argument bounded for large N. αPost gets the same chirp scaled by
    // 1/m so the post-multiply absorbs the IFFT normalisation.
    let invMD = 1.0 / Double(m)
    for k in 0..<n {
      let theta = Double.pi * Double((k * k) % (2 * n)) / Double(n)
      let c = cos(theta)
      let s = -sin(theta)
      alphaRe[k] = c
      alphaIm[k] = s
      alphaPostRe[k] = c * invMD
      alphaPostIm[k] = s * invMD
    }

    // Build the b-sequence (length m, real+imag temp), then FFT once.
    //   b[0]   = 1
    //   b[k]   = exp(+iπk²/N)            for k = 1..n-1
    //   b[m-k] = b[k]                    (symmetry around 0)
    //   b[k]   = 0                       elsewhere
    let bRe = UnsafeMutablePointer<Double>.allocate(capacity: m)
    let bIm = UnsafeMutablePointer<Double>.allocate(capacity: m)
    defer {
      bRe.deallocate()
      bIm.deallocate()
    }
    bRe.update(repeating: 0, count: m)
    bIm.update(repeating: 0, count: m)
    bRe[0] = 1
    for k in 1..<n {
      let theta = Double.pi * Double((k * k) % (2 * n)) / Double(n)
      let c = cos(theta)
      let s = sin(theta)
      bRe[k] = c
      bIm[k] = s
      bRe[m - k] = c
      bIm[m - k] = s
    }
    vDSP_DFT_ExecuteD(fwd, bRe, bIm, bRealF, bImagF)

    // Defensive zero-init of scratch (avoids stale-NaN propagation if a caller
    // uses uninitialised tail bytes).
    aRe.update(repeating: 0, count: m)
    aIm.update(repeating: 0, count: m)
    aReF.update(repeating: 0, count: m)
    aImF.update(repeating: 0, count: m)
    pRe.update(repeating: 0, count: m)
    pIm.update(repeating: 0, count: m)
    cRe.update(repeating: 0, count: m)
    cIm.update(repeating: 0, count: m)
  }

  deinit {
    vDSP_DFT_DestroySetupD(fftFwd)
    vDSP_DFT_DestroySetupD(fftInv)
    alphaRe.deallocate()
    alphaIm.deallocate()
    alphaPostRe.deallocate()
    alphaPostIm.deallocate()
    bRealF.deallocate()
    bImagF.deallocate()
    aRe.deallocate()
    aIm.deallocate()
    aReF.deallocate()
    aImF.deallocate()
    pRe.deallocate()
    pIm.deallocate()
    cRe.deallocate()
    cIm.deallocate()
  }

  /// Run the N-point DFT. `inverse=false` is the forward transform;
  /// `inverse=true` is the unnormalised inverse — caller divides by `n` if
  /// the textbook 1/N normalisation is wanted.
  ///
  /// Implementation: `IDFT(x) = conj(DFT(conj(x)))`, which lets the inverse
  /// path reuse the forward `α` and `bRealF/bImagF` tables — pre-multiply
  /// with `Conjugate=-1` (vDSP applies conj to B), post-multiply regular,
  /// then negate the imag of the output.
  ///
  /// vDSP convention for `vDSP_zvmulD`'s `Conjugate` arg: `+1` = `C = A · B`,
  /// `-1` = `C = A · conj(B)`.
  func execute(
    realIn: UnsafePointer<Double>, imagIn: UnsafePointer<Double>,
    realOut: UnsafeMutablePointer<Double>, imagOut: UnsafeMutablePointer<Double>,
    inverse: Bool
  ) {
    // Step 1: a[0..n) = α · x (forward) or α · conj(x) (inverse).
    // Tried `vDSP_zvmulD` here too — it benchmarks slower than this scalar
    // loop because the compiler already vectorises the simple form and the
    // vDSP per-call setup dominates for n ≈ 2k. Keeping scalar.
    let conjSign: Double = inverse ? -1.0 : 1.0
    for k in 0..<n {
      let xr = realIn[k]
      let xi = imagIn[k] * conjSign
      let ar = alphaRe[k]
      let ai = alphaIm[k]
      aRe[k] = xr * ar - xi * ai
      aIm[k] = xr * ai + xi * ar
    }

    // Zero-pad the rest of `a` up to length m.
    if m > n {
      (aRe + n).update(repeating: 0, count: m - n)
      (aIm + n).update(repeating: 0, count: m - n)
    }

    // Step 2: cyclic convolution via FFT — A = FFT(a); P = A · B;
    // c = IFFT(P) / m.
    vDSP_DFT_ExecuteD(fftFwd, aRe, aIm, aReF, aImF)
    var aFSplit = DSPDoubleSplitComplex(realp: aReF, imagp: aImF)
    var bSplit = DSPDoubleSplitComplex(realp: bRealF, imagp: bImagF)
    var pSplit = DSPDoubleSplitComplex(realp: pRe, imagp: pIm)
    vDSP_zvmulD(&aFSplit, 1, &bSplit, 1, &pSplit, 1, vDSP_Length(m), 1)
    vDSP_DFT_ExecuteD(fftInv, pRe, pIm, cRe, cIm)
    // The IFFT's missing `1/m` scale is folded into `alphaPost`, so no
    // separate vDSP_vsmulD is needed here.

    // Step 3: post-multiply, write to caller's output.
    //   forward: out = α' · c           (α' = α/m)
    //   inverse: out = conj(α' · c) — negate imag after the regular product.
    var alphaPostSplit = DSPDoubleSplitComplex(realp: alphaPostRe, imagp: alphaPostIm)
    var cSplit = DSPDoubleSplitComplex(realp: cRe, imagp: cIm)
    var outSplit = DSPDoubleSplitComplex(realp: realOut, imagp: imagOut)
    vDSP_zvmulD(&alphaPostSplit, 1, &cSplit, 1, &outSplit, 1, vDSP_Length(n), 1)
    if inverse {
      vDSP_vnegD(imagOut, 1, imagOut, 1, vDSP_Length(n))
    }
  }
}
