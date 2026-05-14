// Shared interface for any complex-input/output DFT engine. The
// `ComplexInnerRealFFT` real-FFT backend takes one of these as its
// inner transform; `BluesteinRealFFT.init` does the priority-based
// selection between the available conformers.

import Foundation

/// Common interface for any complex-input/output unscaled DFT.
///
/// Conformers in this module:
///   * `BluesteinFFT` — universal fallback for any `n`.
///   * `MixedRadixFFT` — native, supports `n` whose prime factors are
///     in `{2, 3, 5, 7}`.
///   * `VDSPComplexDFT` — Apple's `vDSP_DFT_zopD`, supports
///     `n = f·2ᵐ` with `f ∈ {1, 3, 5, 15}`, `m ≥ 3`.
///
/// All three return the unscaled DFT in both directions (forward
/// followed by inverse scales the input by `n`), so they're
/// interchangeable as `ComplexInnerRealFFT.inner`.
protocol ArbitraryComplexFFT: AnyObject {
  func execute(
    realIn: UnsafePointer<Double>, imagIn: UnsafePointer<Double>,
    realOut: UnsafeMutablePointer<Double>, imagOut: UnsafeMutablePointer<Double>,
    inverse: Bool
  )
}
