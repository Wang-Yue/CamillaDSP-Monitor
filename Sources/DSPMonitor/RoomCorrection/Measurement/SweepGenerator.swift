// Logarithmic (exponential) sine sweep + Farina inverse filter.
//
// References:
//   - Farina, A. "Simultaneous measurement of impulse response and
//     distortion with a swept-sine technique." AES 108th Convention,
//     Paris, 2000.
//   - Müller, S. & Massarani, P. "Transfer-Function Measurement with
//     Sweeps." JAES 49(6), 2001.
//
// Notation:
//   T  — sweep duration in seconds.
//   f1 — start frequency (Hz). Must be > 0.
//   f2 — end frequency (Hz). Must satisfy f2 > f1 ≤ Nyquist.
//   R  — log-rate = ln(f2/f1) / T. Instantaneous frequency is
//        f_inst(t) = f1 · e^(R·t), exponentially sweeping from f1 to f2.
//   K  — phase scaling = 2π · f1 / R, chosen so the sweep starts
//        with phase 0 and instantaneous frequency f1.
//
//   Sweep:    x(t) = sin( K · (e^(R·t) - 1) ),   t ∈ [0, T]
//   Inverse:  f(t) = x(T - t) · e^(-R·t),         t ∈ [0, T]
//
// The amplitude envelope on the inverse compensates the sweep's pink
// (1/√f) magnitude spectrum so the convolution `x ⊛ f` yields a
// near-Dirac peak. The peak is delayed by T (the sweep length).

import Foundation

enum SweepGenerator {

  /// Generate a logarithmic sine sweep from `f1` Hz to `f2` Hz over
  /// `durationSeconds`, optionally with raised-cosine fade-in /
  /// fade-out tapers to suppress endpoint clicks.
  ///
  /// The output is a unit-amplitude (peak ±1) signal; the caller is
  /// expected to apply playback gain reduction (typically −6 to −12 dB
  /// FS) before sending to the DAC, leaving headroom for the room's
  /// added energy and avoiding clipping in the analog chain.
  static func generate(
    f1: Double,
    f2: Double,
    durationSeconds: Double,
    sampleRate: Int,
    fadeInSeconds: Double = 0.05,
    fadeOutSeconds: Double = 0.05
  ) -> [Double] {
    precondition(f1 > 0, "SweepGenerator: f1 must be > 0")
    precondition(f2 > f1, "SweepGenerator: f2 must be > f1")
    precondition(durationSeconds > 0, "SweepGenerator: durationSeconds must be > 0")
    precondition(sampleRate > 0, "SweepGenerator: sampleRate must be > 0")
    precondition(
      f2 <= Double(sampleRate) / 2.0,
      "SweepGenerator: f2 must be ≤ Nyquist (\(sampleRate/2) Hz), got \(f2)")

    let n = Int((durationSeconds * Double(sampleRate)).rounded())
    let actualT = Double(n) / Double(sampleRate)
    let r = log(f2 / f1) / actualT
    let k = 2.0 * Double.pi * f1 / r

    var sweep = [Double](repeating: 0, count: n)
    let invFs = 1.0 / Double(sampleRate)
    for i in 0..<n {
      let t = Double(i) * invFs
      sweep[i] = sin(k * (exp(r * t) - 1.0))
    }
    applyTapers(
      &sweep,
      fadeInSamples: Int(fadeInSeconds * Double(sampleRate)),
      fadeOutSamples: Int(fadeOutSeconds * Double(sampleRate)))
    return sweep
  }

  /// Generate the matched Farina inverse filter for a sweep created
  /// with the same `f1`, `f2`, `durationSeconds`, `sampleRate`. The
  /// inverse is the time-reversed sweep with an exponentially decaying
  /// envelope that whitens its magnitude spectrum.
  ///
  /// Convolving the captured sweep with the inverse yields the
  /// system's impulse response, with the main peak appearing roughly
  /// at sample index `T · sampleRate` (i.e. at the end of where the
  /// inverse-filter contribution finishes feeding into the
  /// convolution).
  static func inverseFilter(
    f1: Double,
    f2: Double,
    durationSeconds: Double,
    sampleRate: Int
  ) -> [Double] {
    // Reuse `generate` (with no taper) so the inverse is the exact
    // mathematical reverse of the same sweep waveform — any tapering
    // is the user's responsibility on the captured side.
    let sweep = generate(
      f1: f1, f2: f2,
      durationSeconds: durationSeconds, sampleRate: sampleRate,
      fadeInSeconds: 0, fadeOutSeconds: 0)
    let n = sweep.count
    let actualT = Double(n) / Double(sampleRate)
    let r = log(f2 / f1) / actualT
    let invFs = 1.0 / Double(sampleRate)

    var inv = [Double](repeating: 0, count: n)
    for i in 0..<n {
      let t = Double(i) * invFs
      inv[i] = sweep[n - 1 - i] * exp(-r * t)
    }
    return inv
  }

  /// Convenience: generate both the sweep and its inverse with a
  /// single call (the inverse-filter computation is cheap, but bundling
  /// keeps callers from accidentally passing mismatched parameters).
  static func sweepAndInverse(
    f1: Double,
    f2: Double,
    durationSeconds: Double,
    sampleRate: Int,
    fadeInSeconds: Double = 0.05,
    fadeOutSeconds: Double = 0.05
  ) -> (sweep: [Double], inverse: [Double]) {
    let sweep = generate(
      f1: f1, f2: f2,
      durationSeconds: durationSeconds, sampleRate: sampleRate,
      fadeInSeconds: fadeInSeconds, fadeOutSeconds: fadeOutSeconds)
    let inverse = inverseFilter(
      f1: f1, f2: f2,
      durationSeconds: durationSeconds, sampleRate: sampleRate)
    return (sweep, inverse)
  }

  /// Raised-cosine (Hann) tapers at the buffer endpoints. No-op when
  /// the requested taper length is zero.
  private static func applyTapers(
    _ buffer: inout [Double],
    fadeInSamples: Int,
    fadeOutSamples: Int
  ) {
    let fIn = max(0, min(fadeInSamples, buffer.count))
    let fOut = max(0, min(fadeOutSamples, buffer.count))
    if fIn > 0 {
      for i in 0..<fIn {
        let w = 0.5 * (1.0 - cos(Double.pi * Double(i) / Double(fIn)))
        buffer[i] *= w
      }
    }
    if fOut > 0 {
      let n = buffer.count
      for i in 0..<fOut {
        let w = 0.5 * (1.0 - cos(Double.pi * Double(i) / Double(fOut)))
        buffer[n - 1 - i] *= w
      }
    }
  }
}
