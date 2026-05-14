// Time-domain impulse response value type.
//
// Represents the system's response to a unit impulse, recovered by
// `SweepDeconvolver.deconvolve(...)` (sweep convolved with Farina
// inverse) or constructed directly from a measured / synthetic IR.
//
// The `zeroIndex` field marks the sample that corresponds to t = 0 —
// the "now" point of the response. Sweep deconvolution produces an IR
// whose main peak sits at sample `T · sampleRate`, so callers
// typically locate that peak (`peakIndex()`) and slide `zeroIndex`
// onto it before any analysis. Pre-peak samples are causality
// violations (system response before stimulus) and should be small —
// large pre-ring usually indicates an issue with the measurement.

import DSPAudio
import Foundation

public struct ImpulseResponse: Sendable {
  public var samples: [PrcFmt]
  public let sampleRate: Int
  /// Sample index treated as t = 0. Defaults to 0; sweep deconvolution
  /// will set this to the located peak after `centeredOnPeak()`.
  public var zeroIndex: Int

  public init(samples: [PrcFmt], sampleRate: Int, zeroIndex: Int = 0) {
    precondition(sampleRate > 0, "ImpulseResponse: sampleRate must be > 0")
    self.samples = samples
    self.sampleRate = sampleRate
    self.zeroIndex = zeroIndex
  }

  public var count: Int { samples.count }

  /// Index of the sample with the largest absolute value. For a clean
  /// sweep deconvolution this is the main impulse peak.
  public func peakIndex() -> Int {
    guard !samples.isEmpty else { return 0 }
    var best = 0
    var bestVal = abs(samples[0])
    for i in 1..<samples.count {
      let v = abs(samples[i])
      if v > bestVal {
        bestVal = v
        best = i
      }
    }
    return best
  }

  /// Return a copy whose `zeroIndex` is set to the located peak.
  public func centeredOnPeak() -> ImpulseResponse {
    var out = self
    out.zeroIndex = peakIndex()
    return out
  }

  /// Cosine-tapered window around `zeroIndex`. Extracts
  /// `leftSamples` before and `rightSamples` after, applies a
  /// `taperFraction`-wide raised-cosine taper at each end, and
  /// returns a new IR with `zeroIndex == leftSamples`.
  ///
  /// `taperFraction` is the fraction of `leftSamples` (or
  /// `rightSamples`) consumed by the taper at each side; 0 disables
  /// the taper, 0.5 is a half-Hann that meets in the middle.
  public func windowed(
    leftSamples: Int,
    rightSamples: Int,
    taperFraction: PrcFmt = 0.1
  ) -> ImpulseResponse {
    precondition(leftSamples >= 0 && rightSamples >= 0)
    precondition(taperFraction >= 0 && taperFraction <= 1)

    let n = leftSamples + rightSamples
    var out = [PrcFmt](repeating: 0, count: n)
    let srcStart = zeroIndex - leftSamples
    for i in 0..<n {
      let src = srcStart + i
      if src >= 0 && src < samples.count {
        out[i] = samples[src]
      }
    }
    let leftTaper = Int(PrcFmt(leftSamples) * taperFraction)
    let rightTaper = Int(PrcFmt(rightSamples) * taperFraction)
    for i in 0..<min(leftTaper, n) {
      let w = 0.5 * (1.0 - cos(PrcFmt.pi * PrcFmt(i) / PrcFmt(leftTaper)))
      out[i] *= w
    }
    for i in 0..<min(rightTaper, n) {
      let w = 0.5 * (1.0 - cos(PrcFmt.pi * PrcFmt(i) / PrcFmt(rightTaper)))
      out[n - 1 - i] *= w
    }
    return ImpulseResponse(samples: out, sampleRate: sampleRate, zeroIndex: leftSamples)
  }

  /// Computes the Schroeder reverse-integrated energy decay curve from the
  /// impulse peak to the end of the response. Returns values in dB normalized
  /// to 0 dB at the peak.
  public func schroederDecay() -> [PrcFmt] {
    let p = zeroIndex
    guard p < samples.count else { return [] }
    let n = samples.count - p
    var energy = [PrcFmt](repeating: 0, count: n)
    var sum: PrcFmt = 0
    for i in (0..<n).reversed() {
      let s = samples[p + i]
      sum += s * s
      energy[i] = sum
    }
    guard sum > 0 else { return [] }
    let invTotal = 1.0 / sum
    return energy.map { e in
      let ratio = max(e * invTotal, 1e-12)
      return 10.0 * log10(ratio)
    }
  }

  /// Estimates the RT60 decay time in seconds using the Schroeder decay curve.
  /// Fits a linear slope to the decay between `startDB` (e.g. -5 dB) and `endDB` (e.g. -25 dB).
  public func rt60(startDB: PrcFmt = -5.0, endDB: PrcFmt = -25.0) -> PrcFmt {
    let decay = schroederDecay()
    guard decay.count > 1 else { return 0 }

    var idxStart: Int? = nil
    var idxEnd: Int? = nil
    for i in 0..<decay.count {
      if idxStart == nil, decay[i] <= startDB { idxStart = i }
      if idxEnd == nil, decay[i] <= endDB { idxEnd = i }
    }
    guard let s = idxStart, let e = idxEnd, e > s else { return 0 }

    let dt = PrcFmt(e - s) / PrcFmt(sampleRate)
    let dDb = decay[s] - decay[e]
    guard dDb > 0 else { return 0 }

    return dt * (60.0 / dDb)
  }
}
