// PEQ auto-fit correctness tests.
//
// Strategy: synthesise a "measured" magnitude response by evaluating
// a known biquad chain on the log-freq grid, then fit. The fit's
// residual against the target curve should drop close to zero.
//
// Because the fit is greedy and band-by-band rather than a joint
// nonlinear optimisation, we don't require it to recover the original
// biquad parameters — only that the post-fit residual is small.

import Foundation
import Testing

@testable import DSPAudio
@testable import DSPConfig
@testable import DSPFilters
@testable import DSPMeasurement

@Suite struct PEQAutoFitTests {

  private let sampleRate = 48000

  /// Evaluate a chain of biquads at the given frequencies, summing
  /// gain in dB.
  private func evaluateChain(
    _ params: [BiquadParameters], at frequencies: [PrcFmt]
  ) -> [PrcFmt] {
    var out = [PrcFmt](repeating: 0, count: frequencies.count)
    for p in params {
      guard let coeffs = BiquadCoefficients.compute(parameters: p, sampleRate: sampleRate) else {
        continue
      }
      for i in 0..<frequencies.count {
        out[i] += coeffs.gainDB(atFreqHz: frequencies[i], sampleRate: sampleRate)
      }
    }
    return out
  }

  /// Apply a fit to a measured spectrum, then compute the residual
  /// against the target.
  private func residualAfterFit(
    measured: [PrcFmt], frequencies: [PrcFmt], target: TargetCurve,
    options: PEQAutoFit.Options
  ) -> [PrcFmt] {
    let fitted = PEQAutoFit.fit(
      measuredMagnitudeDB: measured,
      frequencies: frequencies,
      target: target,
      sampleRate: sampleRate,
      options: options)
    let fitGain = evaluateChain(fitted, at: frequencies)
    var residual = [PrcFmt](repeating: 0, count: frequencies.count)
    for i in 0..<frequencies.count {
      residual[i] = (measured[i] + fitGain[i]) - target.evaluate(atFreqHz: frequencies[i])
    }
    return residual
  }

  /// Already-flat input, flat target → no bands placed. Convergence
  /// short-circuits before the first iteration.
  @Test func FlatInputProducesNoBands() {
    let grid = PEQAutoFit.logFrequencyGrid(fMin: 20, fMax: 20_000, count: 256)
    let measured = [PrcFmt](repeating: 0, count: grid.count)
    let bands = PEQAutoFit.fit(
      measuredMagnitudeDB: measured,
      frequencies: grid,
      target: .flat,
      sampleRate: sampleRate)
    #expect(bands.isEmpty, "expected empty fit for flat input; got \(bands.count) bands")
  }

  /// Recover a single +6 dB peaking bump at 1 kHz: feed the chain's
  /// FR as the "measured" spectrum, fit with a single band targeting
  /// flat, and verify the post-fit residual is small in-band.
  @Test func RecoversSingleBumpToFlat() {
    let grid = PEQAutoFit.logFrequencyGrid(fMin: 20, fMax: 20_000, count: 512)
    let truth = [BiquadParameters(type: .peaking, freq: 1000, gain: 6, q: 2.0)]
    let measured = evaluateChain(truth, at: grid)

    let opts = PEQAutoFit.Options(bandCount: 2)
    let residual = residualAfterFit(
      measured: measured, frequencies: grid, target: .flat, options: opts)

    // Peak residual should be small inside the audio band where the
    // original bump lives. We allow some slack near the band edges
    // where biquad responses can have non-trivial leakage.
    var maxInBand: PrcFmt = 0
    for i in 0..<grid.count where grid[i] >= 100 && grid[i] <= 10_000 {
      maxInBand = max(maxInBand, abs(residual[i]))
    }
    #expect(maxInBand < 1.5, "post-fit max in-band residual \(maxInBand) dB > 1.5")
  }

  /// Three-band synthetic chain: −4 dB at 100 Hz, +6 dB at 1 kHz,
  /// −3 dB at 5 kHz. Five bands of fitting headroom should be enough
  /// for the greedy algorithm to land within ~2 dB across the band.
  @Test func RecoversThreeBumpsToFlat() {
    let grid = PEQAutoFit.logFrequencyGrid(fMin: 20, fMax: 20_000, count: 512)
    let truth: [BiquadParameters] = [
      .init(type: .peaking, freq: 100, gain: -4, q: 1.5),
      .init(type: .peaking, freq: 1000, gain: 6, q: 2.0),
      .init(type: .peaking, freq: 5000, gain: -3, q: 1.5),
    ]
    let measured = evaluateChain(truth, at: grid)
    let opts = PEQAutoFit.Options(bandCount: 5)
    let residual = residualAfterFit(
      measured: measured, frequencies: grid, target: .flat, options: opts)

    var maxInBand: PrcFmt = 0
    for i in 0..<grid.count where grid[i] >= 200 && grid[i] <= 10_000 {
      maxInBand = max(maxInBand, abs(residual[i]))
    }
    #expect(maxInBand < 2.0, "post-fit max in-band residual \(maxInBand) dB > 2.0")
  }

  /// Fitter must respect the gain cap. Feed an extreme +30 dB peak;
  /// the resulting band's gain should not exceed `maxGainDB` (and the
  /// residual stays large because we couldn't fully correct).
  @Test func RespectsMaxGainCap() {
    let grid = PEQAutoFit.logFrequencyGrid(fMin: 20, fMax: 20_000, count: 256)
    let truth = [BiquadParameters(type: .peaking, freq: 1000, gain: 30, q: 4)]
    let measured = evaluateChain(truth, at: grid)
    let opts = PEQAutoFit.Options(bandCount: 1, maxGainDB: 12)
    let bands = PEQAutoFit.fit(
      measuredMagnitudeDB: measured,
      frequencies: grid,
      target: .flat,
      sampleRate: sampleRate,
      options: opts)
    #expect(bands.count == 1)
    let placedGain = abs(bands[0].gain ?? 0)
    #expect(placedGain <= 12.0 + 1e-9, "placed gain \(placedGain) exceeded 12 dB cap")
  }
}
