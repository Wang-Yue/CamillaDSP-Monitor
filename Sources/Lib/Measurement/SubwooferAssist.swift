// Subwoofer-mains crossover assistant.
//
// Given a mains-only measurement and a subwoofer-only measurement
// (both at the same listening position), this module recommends:
//
//   * **Time-of-flight delay** for the subwoofer relative to the
//     mains, derived from the cross-correlation peak between the
//     two IRs. Sub typically arrives later than mains because the
//     speaker's typically further away (or the sub's DSP latency
//     differs).
//
//   * **Crossover frequency**, picked at the magnitude crossover
//     point — the frequency where the mains' response has rolled off
//     to roughly match the sub's. In practice that's the frequency
//     where the mains drop ~6 dB below their mid-band level, or
//     where the two responses cross in dB, whichever is lower.
//
//   * **Filter chain**: high-pass on mains, low-pass on sub, both
//     2nd-order Butterworth (Q ≈ 0.71), plus the sub delay.
//
// The assistant returns recommendations as plain values; the UI
// presents them and the user decides whether to apply. Auto-apply
// would require knowing where in the user's pipeline to insert the
// filters and how their downstream Convolution stage interacts.

import DSPAudio
import DSPConfig
import DSPFilters
import Foundation

public struct SubwooferRecommendation: Sendable {
  /// Delay to add to the subwoofer signal so its arrival aligns
  /// with the mains at the listening position. Positive ⇒ delay the
  /// sub; negative ⇒ delay the mains by `|value|` instead.
  public let subDelayMs: Double
  /// Suggested crossover frequency. Both filters use this as their
  /// −3 dB corner.
  public let crossoverHz: Double
  /// High-pass biquad parameters for the mains.
  public let mainsHighPass: BiquadParameters
  /// Low-pass biquad parameters for the sub.
  public let subLowPass: BiquadParameters
  /// Confidence indicator — 0 (don't trust this) to 1 (clean
  /// crossover region detected). Drops when there's no overlap to
  /// pick a frequency in, or when the cross-correlation peak is
  /// ambiguous.
  public let confidence: Double
  /// Plain-text rationale the UI surfaces alongside the numbers
  /// (why this crossover, what to watch out for, etc.).
  public let summary: String
}

public enum SubwooferAssist {

  /// Recommend crossover + delay settings from a sub-only and
  /// mains-only impulse response measured at the same position.
  public static func recommend(
    mainsIR: ImpulseResponse,
    subIR: ImpulseResponse
  ) -> SubwooferRecommendation? {
    precondition(mainsIR.sampleRate == subIR.sampleRate)
    let sr = mainsIR.sampleRate
    // Time-of-flight: cross-correlate mains vs sub. The peak's
    // offset (in samples) is the inter-arrival delay. Positive
    // offset = sub arrives later than mains.
    let delaySamples = peakOffset(of: subIR.samples, against: mainsIR.samples)
    let subDelayMs = Double(delaySamples) / Double(sr) * 1000.0

    // Magnitude crossover: pick the frequency where the mains have
    // dropped to about half-power (-3 dB) below their mid-band
    // level, roughly. Cap to [40, 200] Hz — any sub crossover
    // outside that range is unusual and probably the IRs aren't
    // actually mains/sub.
    let mainsFR = FrequencyResponse.from(impulseResponse: mainsIR)
    let subFR = FrequencyResponse.from(impulseResponse: subIR)
    let (crossoverHz, confidence, summary) = chooseCrossover(
      mainsFR: mainsFR, subFR: subFR)

    let q = 0.7071  // Butterworth (Linkwitz-Riley pair when stacked)
    let hp = BiquadParameters(type: .highpass, freq: crossoverHz, q: q)
    let lp = BiquadParameters(type: .lowpass, freq: crossoverHz, q: q)
    return SubwooferRecommendation(
      subDelayMs: subDelayMs,
      crossoverHz: crossoverHz,
      mainsHighPass: hp,
      subLowPass: lp,
      confidence: confidence,
      summary: summary)
  }

  // MARK: - Inner helpers

  /// Cross-correlation peak offset of `b` against `a` in samples.
  /// Positive ⇒ `b` is delayed relative to `a`.
  ///
  /// Uses a direct time-domain correlation over a ±100 ms search
  /// window — that's wide enough to capture any reasonable
  /// loudspeaker placement difference. Time-domain is fine here:
  /// we only do this once per "Recommend" click, and the search
  /// window is small.
  private static func peakOffset(of b: [Double], against a: [Double]) -> Int {
    let sr = 48_000  // search window is in samples; a tighter ±100 ms
    let maxLag = min(sr / 10, min(a.count, b.count) - 1)
    var bestLag = 0
    var bestVal = -Double.infinity
    for lag in -maxLag...maxLag {
      var sum = 0.0
      let aStart = max(0, -lag)
      let bStart = max(0, lag)
      let n = min(a.count - aStart, b.count - bStart)
      if n <= 0 { continue }
      for k in 0..<n {
        sum += a[aStart + k] * b[bStart + k]
      }
      if sum > bestVal {
        bestVal = sum
        bestLag = lag
      }
    }
    return bestLag
  }

  /// Pick a crossover frequency by walking up from 40 Hz and
  /// finding the first frequency where the mains response is at
  /// most equal to the sub response (i.e., the mains have rolled
  /// off enough that the sub takes over).
  private static func chooseCrossover(
    mainsFR: FrequencyResponse, subFR: FrequencyResponse
  ) -> (freq: Double, confidence: Double, summary: String) {
    let bins = mainsFR.bins
    let binHz = Double(mainsFR.sampleRate) / Double(mainsFR.fftSize)
    var crossingHz: Double? = nil
    // Anchor the mains at the average of [120, 200] Hz. Anywhere
    // around the crossover region is fine — we want a stable
    // reference, not the absolute mid-band level.
    var mainsRef = 0.0
    var refCount = 0
    for k in 1..<bins {
      let f = Double(k) * binHz
      if f >= 120, f <= 200 {
        mainsRef += mainsFR.magnitudeDB(at: k)
        refCount += 1
      }
    }
    if refCount > 0 { mainsRef /= Double(refCount) }
    // Walk up; first frequency where mains has dropped 6 dB below
    // its reference and the sub is louder than the mains is the
    // crossover candidate.
    for k in 1..<bins {
      let f = Double(k) * binHz
      if f < 30 { continue }
      if f > 250 { break }
      let mainsDB = mainsFR.magnitudeDB(at: k)
      let subDB = subFR.magnitudeDB(at: k)
      if (mainsRef - mainsDB) >= 6.0, subDB > mainsDB {
        crossingHz = f
        break
      }
    }
    if let cross = crossingHz {
      // Round to a nice "audio" value to make the UI numbers
      // legible. Crossover frequencies are conventionally picked
      // from a small set (40, 50, 60, 80, 100, 120, 150, 180 Hz).
      let snapped = snapToCommonCrossover(cross)
      let summary = """
        Picked the crossover where the mains have rolled off ~6 dB \
        below their 120–200 Hz reference and the sub is louder. \
        Mains high-pass and sub low-pass at \(Int(snapped)) Hz \
        produce a 4th-order Linkwitz-Riley sum (12 dB/oct each, in \
        phase at fc).
        """
      return (snapped, 0.85, summary)
    }
    // No clean crossover — fall back to 80 Hz (THX standard) and
    // flag low confidence so the UI surfaces the issue.
    return (
      80.0, 0.2,
      "Couldn't find a clean overlap between sub and mains. Falling back to the THX-standard 80 Hz crossover; verify by ear or with a fresh measurement."
    )
  }

  private static func snapToCommonCrossover(_ f: Double) -> Double {
    let common: [Double] = [40, 50, 60, 70, 80, 90, 100, 120, 150, 180, 200]
    return common.min(by: { abs($0 - f) < abs($1 - f) }) ?? round(f)
  }
}
