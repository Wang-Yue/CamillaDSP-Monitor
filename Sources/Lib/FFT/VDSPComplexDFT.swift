// vDSP `DFT_zopD` backend for complex DFTs at sizes `f·2ᵐ`,
// `f ∈ {1, 3, 5, 15}`, `m ≥ 3`. Used by `ComplexInnerRealFFT` as its
// inner transform when the size qualifies — Apple's tuned mixed-radix
// is typically faster than `MixedRadixFFT` in this regime.

import Accelerate
import Foundation

/// Wraps `vDSP_DFT_zopD` (complex out-of-place DFT). Setup creation
/// returns `nil` for any size outside the supported family, in which
/// case the caller (`BluesteinRealFFT.init`) falls back to
/// `MixedRadixFFT` (small-prime sizes 2/3/5/7) or `BluesteinFFT`
/// (universal).
///
/// Output convention: unscaled DFT in both directions (round-trip
/// scales the input by `n`), matching `MixedRadixFFT` and
/// `BluesteinFFT` — drop-in for `ComplexInnerRealFFT.inner`.
final class VDSPComplexDFT: ArbitraryComplexFFT {
  private let setupForward: vDSP_DFT_SetupD
  private let setupInverse: vDSP_DFT_SetupD

  init?(n: Int) {
    guard
      let fwd = vDSP_DFT_zop_CreateSetupD(nil, vDSP_Length(n), .FORWARD)
    else { return nil }
    guard
      let inv = vDSP_DFT_zop_CreateSetupD(fwd, vDSP_Length(n), .INVERSE)
    else {
      vDSP_DFT_DestroySetupD(fwd)
      return nil
    }
    self.setupForward = fwd
    self.setupInverse = inv
  }

  deinit {
    // Documented as safe in either order; destroying the inverse first
    // mirrors creation order in reverse.
    vDSP_DFT_DestroySetupD(setupInverse)
    vDSP_DFT_DestroySetupD(setupForward)
  }

  func execute(
    realIn: UnsafePointer<Double>, imagIn: UnsafePointer<Double>,
    realOut: UnsafeMutablePointer<Double>, imagOut: UnsafeMutablePointer<Double>,
    inverse: Bool
  ) {
    let setup = inverse ? setupInverse : setupForward
    vDSP_DFT_ExecuteD(setup, realIn, imagIn, realOut, imagOut)
  }
}
