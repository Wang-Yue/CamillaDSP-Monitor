// One-shot FFT-domain convolution of a captured sweep with the
// Farina inverse filter, producing the system's impulse response.
//
// The math is "convolution" but the operational name is
// "deconvolution" because that's what it does to the sweep — given
// `y(t) = x(t) ⊛ h(t)` (capture = sweep ⊛ system IR) and the matched
// inverse `f(t)` such that `x(t) ⊛ f(t) ≈ δ(t - T)`, computing
// `y(t) ⊛ f(t)` yields `h(t - T)`. The resulting peak appears around
// sample `T · sampleRate − 1` (where `T` is the sweep duration);
// `centeredOnPeak()` slides `zeroIndex` onto it for downstream
// analysis.
//
// This is a single-pass batch op, not a streaming filter — the
// captured signal is finite, so we transform once at the full
// `M = capture + inverse − 1` length (rounded up to the next even
// number for `RealFFT`). Streaming overlap-save isn't needed.

import Accelerate
import Foundation

public enum SweepDeconvolver {

  /// Convolve `captured` with `inverseFilter` via FFT, returning the
  /// raw IR samples of length `captured.count + inverseFilter.count − 1`.
  ///
  /// For the typical sweep workflow, prefer `deconvolve(captured:
  /// f1:f2:durationSeconds:sampleRate:)` which builds the matched
  /// inverse and returns a properly-centred `ImpulseResponse`.
  public static func convolve(
    _ captured: [Double],
    with inverseFilter: [Double]
  ) -> [Double] {
    precondition(!captured.isEmpty, "SweepDeconvolver: captured must be non-empty")
    precondition(!inverseFilter.isEmpty, "SweepDeconvolver: inverseFilter must be non-empty")

    let m = captured.count + inverseFilter.count - 1
    func nextPowerOfTwo(_ val: Int) -> Int {
      var p = 8
      while p < val { p *= 2 }
      return p
    }
    let n = nextPowerOfTwo(max(8, m))
    let bins = n / 2 + 1
    let fft = MeasurementFFT(length: n)

    var aPadded = [Double](repeating: 0, count: n)
    var bPadded = [Double](repeating: 0, count: n)
    var outBuf = [Double](repeating: 0, count: n)
    var aRe = [Double](repeating: 0, count: bins)
    var aIm = [Double](repeating: 0, count: bins)
    var bRe = [Double](repeating: 0, count: bins)
    var bIm = [Double](repeating: 0, count: bins)
    var cRe = [Double](repeating: 0, count: bins)
    var cIm = [Double](repeating: 0, count: bins)

    aPadded.replaceSubrange(0..<captured.count, with: captured)
    bPadded.replaceSubrange(0..<inverseFilter.count, with: inverseFilter)

    fft.forward(realIn: aPadded, specRe: &aRe, specIm: &aIm)
    fft.forward(realIn: bPadded, specRe: &bRe, specIm: &bIm)

    zvmulD(ar: aRe, ai: aIm, br: bRe, bi: bIm, cr: &cRe, ci: &cIm, length: bins)

    fft.inverse(specRe: cRe, specIm: cIm, realOut: &outBuf)

    let invN = 1.0 / Double(n)
    var resultPadded = [Double](repeating: 0, count: n)
    vDSP.multiply(invN, outBuf, result: &resultPadded)
    return Array(resultPadded[0..<m])
  }

  /// Build the matched Farina inverse for `(f1, f2, durationSeconds)`,
  /// convolve `captured` with it, and return the IR with `zeroIndex`
  /// centred on the located peak.
  ///
  /// `captured` is expected to be the recording of the sweep played
  /// through the system under test (mic + room + DAC + speaker, in
  /// the room-correction case). It must be at least `T · sampleRate`
  /// samples long; trailing silence on the capture side is fine and
  /// becomes IR tail.
  public static func deconvolve(
    captured: [Double],
    f1: Double,
    f2: Double,
    durationSeconds: Double,
    sampleRate: Int
  ) -> ImpulseResponse {
    let inverse = SweepGenerator.inverseFilter(
      f1: f1, f2: f2, durationSeconds: durationSeconds, sampleRate: sampleRate)
    let raw = convolve(captured, with: inverse)
    return ImpulseResponse(samples: raw, sampleRate: sampleRate).centeredOnPeak()
  }

  private static func zvmulD(
    ar: UnsafePointer<Double>, ai: UnsafePointer<Double>,
    br: UnsafePointer<Double>, bi: UnsafePointer<Double>,
    cr: UnsafeMutablePointer<Double>, ci: UnsafeMutablePointer<Double>,
    length: Int
  ) {
    var aSplit = DSPDoubleSplitComplex(
      realp: UnsafeMutablePointer(mutating: ar), imagp: UnsafeMutablePointer(mutating: ai))
    var bSplit = DSPDoubleSplitComplex(
      realp: UnsafeMutablePointer(mutating: br), imagp: UnsafeMutablePointer(mutating: bi))
    var cSplit = DSPDoubleSplitComplex(realp: cr, imagp: ci)
    vDSP_zvmulD(&aSplit, 1, &bSplit, 1, &cSplit, 1, vDSP_Length(length), 1)
  }
}
