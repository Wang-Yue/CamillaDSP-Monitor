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
import DSPAudio
import DSPFFT
import Foundation

public enum SweepDeconvolver {

  /// Convolve `captured` with `inverseFilter` via FFT, returning the
  /// raw IR samples of length `captured.count + inverseFilter.count − 1`.
  ///
  /// For the typical sweep workflow, prefer `deconvolve(captured:
  /// f1:f2:durationSeconds:sampleRate:)` which builds the matched
  /// inverse and returns a properly-centred `ImpulseResponse`.
  public static func convolve(
    _ captured: [PrcFmt],
    with inverseFilter: [PrcFmt]
  ) -> [PrcFmt] {
    precondition(!captured.isEmpty, "SweepDeconvolver: captured must be non-empty")
    precondition(!inverseFilter.isEmpty, "SweepDeconvolver: inverseFilter must be non-empty")

    let m = captured.count + inverseFilter.count - 1
    // RealFFT requires an even length; round up.
    let n = m + (m % 2)
    let bins = n / 2 + 1
    let fft = RealFFT(length: n)

    let aPadded = UnsafeMutablePointer<PrcFmt>.allocate(capacity: n)
    let bPadded = UnsafeMutablePointer<PrcFmt>.allocate(capacity: n)
    let outBuf = UnsafeMutablePointer<PrcFmt>.allocate(capacity: n)
    let aRe = UnsafeMutablePointer<PrcFmt>.allocate(capacity: bins)
    let aIm = UnsafeMutablePointer<PrcFmt>.allocate(capacity: bins)
    let bRe = UnsafeMutablePointer<PrcFmt>.allocate(capacity: bins)
    let bIm = UnsafeMutablePointer<PrcFmt>.allocate(capacity: bins)
    let cRe = UnsafeMutablePointer<PrcFmt>.allocate(capacity: bins)
    let cIm = UnsafeMutablePointer<PrcFmt>.allocate(capacity: bins)
    aPadded.initialize(repeating: 0, count: n)
    bPadded.initialize(repeating: 0, count: n)
    outBuf.initialize(repeating: 0, count: n)
    aRe.initialize(repeating: 0, count: bins)
    aIm.initialize(repeating: 0, count: bins)
    bRe.initialize(repeating: 0, count: bins)
    bIm.initialize(repeating: 0, count: bins)
    cRe.initialize(repeating: 0, count: bins)
    cIm.initialize(repeating: 0, count: bins)
    defer {
      aPadded.deinitialize(count: n)
      aPadded.deallocate()
      bPadded.deinitialize(count: n)
      bPadded.deallocate()
      outBuf.deinitialize(count: n)
      outBuf.deallocate()
      aRe.deinitialize(count: bins)
      aRe.deallocate()
      aIm.deinitialize(count: bins)
      aIm.deallocate()
      bRe.deinitialize(count: bins)
      bRe.deallocate()
      bIm.deinitialize(count: bins)
      bIm.deallocate()
      cRe.deinitialize(count: bins)
      cRe.deallocate()
      cIm.deinitialize(count: bins)
      cIm.deallocate()
    }

    captured.withUnsafeBufferPointer { src in
      if let base = src.baseAddress {
        aPadded.update(from: base, count: captured.count)
      }
    }
    inverseFilter.withUnsafeBufferPointer { src in
      if let base = src.baseAddress {
        bPadded.update(from: base, count: inverseFilter.count)
      }
    }

    fft.forward(realIn: aPadded, specRe: aRe, specIm: aIm)
    fft.forward(realIn: bPadded, specRe: bRe, specIm: bIm)

    var aSplit = DSPDoubleSplitComplex(realp: aRe, imagp: aIm)
    var bSplit = DSPDoubleSplitComplex(realp: bRe, imagp: bIm)
    var cSplit = DSPDoubleSplitComplex(realp: cRe, imagp: cIm)
    vDSP_zvmulD(&aSplit, 1, &bSplit, 1, &cSplit, 1, vDSP_Length(bins), 1)

    fft.inverse(specRe: cRe, specIm: cIm, realOut: outBuf)

    // RealFFT.inverse multiplies by length; undo so the
    // convolution sum has the conventional unit scaling.
    let invN = 1.0 / PrcFmt(n)
    var result = [PrcFmt](repeating: 0, count: m)
    var scale = invN
    result.withUnsafeMutableBufferPointer { dst in
      if let dstBase = dst.baseAddress {
        vDSP_vsmulD(outBuf, 1, &scale, dstBase, 1, vDSP_Length(m))
      }
    }
    return result
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
    captured: [PrcFmt],
    f1: PrcFmt,
    f2: PrcFmt,
    durationSeconds: PrcFmt,
    sampleRate: Int
  ) -> ImpulseResponse {
    let inverse = SweepGenerator.inverseFilter(
      f1: f1, f2: f2, durationSeconds: durationSeconds, sampleRate: sampleRate)
    let raw = convolve(captured, with: inverse)
    return ImpulseResponse(samples: raw, sampleRate: sampleRate).centeredOnPeak()
  }
}
