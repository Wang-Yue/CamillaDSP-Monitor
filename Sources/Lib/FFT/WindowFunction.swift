// Squared 4-term Blackman-Harris window and the matching windowed-sinc
// kernel builder used by `SynchronousResampler`.
//
// Independently derived from textbook formulations:
//
//   * F. J. Harris (1978), "On the Use of Windows for Harmonic Analysis
//     with the Discrete Fourier Transform", Proc. IEEE, vol. 66, no. 1,
//     pp. 51-83 — Table 6 lists the 4-term Blackman-Harris coefficients
//     used here. The "-92 dB" coefficient row is the standard choice.
//   * A. V. Oppenheim and R. W. Schafer, "Discrete-Time Signal
//     Processing" (Prentice-Hall) — windowed-sinc lowpass filter
//     construction (`h[n] = 2·fc · sinc(2·fc·(n − N/2)) · w[n]`,
//     normalised so DC gain = 1).
//
// The squared variant `w²[n]` is just the pointwise square of the
// 4-term BH window. Squaring the window in time domain corresponds to
// convolving its DTFT with itself in the frequency domain — main lobe
// roughly doubles in width, sidelobe attenuation roughly doubles in
// dB (≈ -184 dB peak), which is what the FFT-based resampler wants for
// a single-pass overlap-add path.

import Foundation

// 4-term Blackman-Harris coefficients (Harris 1978, Table 6).
// `Σ aₖ = 1` so the symmetric window peaks at 1.0; the periodic form
// (used here) matches that at the centre tap, `i = N/2`.
private let bhA0 = 0.35875
private let bhA1 = 0.48829
private let bhA2 = 0.14128
private let bhA3 = 0.01168

/// Sample of the squared 4-term Blackman-Harris window at index `i`
/// of a length-`n` *periodic* window.
///
///     w[i]  = a₀ − a₁·cos(θ) + a₂·cos(2θ) − a₃·cos(3θ),  θ = 2πi/n
///     w²[i] = w[i] · w[i]
///
/// Periodic (divisor `n`, not `n − 1`) so the window is exactly DFT-bin
/// aligned: `w[0]` and the implicit `w[n]` are equal, which keeps the
/// FFT-convolution path free of boundary artifacts.
@inline(__always)
func blackmanHarris2Value(i: Int, n: Int) -> Double {
  let phase = 2.0 * .pi * Double(i) / Double(n)
  let w =
    bhA0
    - bhA1 * cos(phase)
    + bhA2 * cos(2 * phase)
    - bhA3 * cos(3 * phase)
  return w * w
}

/// Width of the BH² windowed-sinc transition band, in cycles/sample
/// (where `1.0 ≡ Nyquist of the rate the kernel operates at`).
///
/// Sizing rationale (Harris 1978, §III; Crochiere & Rabiner 1983, §3;
/// Oppenheim & Schafer §7.5 "Kaiser & related windowed-sinc filters"):
///
///   * The 4-term BH window's main lobe spans ~ 8 DFT bins peak-to-
///     peak (Harris 1978 Table I, "-92 dB" row); the half-width from
///     centre to first null is `4/N` in normalised cycles.
///   * Squaring the time-domain window convolves its DTFT with
///     itself, so BH²'s main-lobe half-width roughly doubles to
///     `8/N`. Peak sidelobe attenuation also roughly doubles in dB
///     to ≈ -184 dB.
///   * The cutoff of a windowed-sinc lowpass sits at the *centre* of
///     the transition band (half-power). `8/N` is the *theoretical*
///     half-main-lobe distance the cutoff would need to retreat from
///     `target_nyquist` if the stopband were a sharp rectangle
///     ending at the first null. Real BH² responses keep rolling
///     off slowly past the main lobe, so the cutoff has to retreat
///     further to fully suppress the residual near-band sidelobes.
///   * The empirical fit `13.5/N + 50/N²` keeps the stopband at
///     ≤ -180 dB across `N ∈ [256, 8192]` while leaving the passband
///     within ~ 0.01 dB of unity. The `50/N²` quadratic correction
///     tightens the fit at short filter lengths (N < 256) and is
///     negligible at the `N ≥ ~1 k` sizes the resampler actually picks.
func transitionMarginBlackmanHarris2(filterLength n: Int) -> Double {
  precondition(n > 0, "filterLength must be positive")
  let nd = Double(n)
  return 13.5 / nd + 50.0 / (nd * nd)
}

/// Heuristic cutoff for the windowed-sinc anti-aliasing filter at
/// length `n`. `targetNyquist` is the highest passband frequency the
/// downstream stage cares about (in cycles/sample, with `1.0 ≡
/// Nyquist of the rate the kernel operates at`):
///
///   * Upsampling — pass the full input bandwidth, so target = 1.0.
///   * Downsampling — pass only up to output Nyquist, so target =
///     `Fₒ/Fᵢ`, which lives in `(0, 1)`.
///
/// The cutoff is `targetNyquist − transitionMargin(n)`, clamped above
/// a small positive epsilon to keep `makeBlackmanHarris2SincKernel`
/// well-behaved for pathologically short filters where the margin
/// would otherwise drag the cutoff to or past zero.
public func cutoffForBlackmanHarris2(filterLength n: Int, targetNyquist: Double = 1.0) -> Double {
  precondition(n > 0, "filterLength must be positive")
  precondition(targetNyquist > 0 && targetNyquist <= 1.0, "targetNyquist must be in (0, 1]")
  let cutoff = targetNyquist - transitionMarginBlackmanHarris2(filterLength: n)
  // Floor at a small positive value — for downsampling, the cutoff
  // legitimately lives anywhere in `(0, 1)` depending on `Fₒ/Fᵢ`.
  return max(1e-6, cutoff)
}

/// Build a unity-DC-gain windowed-sinc lowpass filter of `length` taps
/// using the squared 4-term Blackman-Harris window.
///
///     h[n] = sinc(cutoff·(n − N/2)) · w²[n],   `sinc(x) = sin(πx)/(πx)`
///
/// then divided by `Σh` so DC gain is exactly 1. `cutoff` is in
/// cycles/sample with `1.0 ≡ Nyquist` — i.e., `cutoff = 0.5` is a
/// half-band filter.
///
/// The returned `[Double]` is built once at init by the resampler and
/// FFT'd into the filter spectrum; this function is not on any audio
/// hot path.
public func makeBlackmanHarris2SincKernel(length: Int, cutoff: Double) -> [Double] {
  precondition(length > 0, "kernel length must be positive")
  precondition(cutoff > 0 && cutoff <= 1.0, "cutoff must be in (0, 1]")

  var h = [Double](repeating: 0, count: length)
  let center = Double(length / 2)
  for i in 0..<length {
    let x = (Double(i) - center) * cutoff
    let arg = x * .pi
    // The `1.0` at the singularity is `lim_{x→0} sin(πx)/(πx) = 1`.
    let sincVal: Double = abs(x) < 1e-12 ? 1.0 : sin(arg) / arg
    h[i] = sincVal * blackmanHarris2Value(i: i, n: length)
  }

  // Normalise to unity DC gain. `Σh` is `H(0)` for a real FIR, so this
  // makes `|H(0)| = 1` regardless of cutoff or filter length.
  var sum: Double = 0
  for v in h { sum += v }
  if sum != 0 {
    let inv = 1.0 / sum
    for i in 0..<length { h[i] *= inv }
  }
  return h
}
