// FIRDesign correctness tests.
//
// Verifies that:
//   1. A flat magnitude (empty biquad chain, preamp 0 dB) produces a
//      Kronecker delta IR for both min-phase and linear-phase paths.
//   2. The designed IR's frequency response matches the source biquad
//      chain within a small tolerance across the audio band.
//   3. Min-phase IRs are causal — energy is concentrated near n = 0
//      and falls off rapidly.
//   4. Linear-phase IRs are symmetric around their centre.

import Accelerate
import Foundation
import Testing

@testable import DSPAudio
@testable import DSPConfig
@testable import DSPFilters
@testable import DSPMeasurement

@Suite struct FIRDesignTests {

  private let sampleRate = 48000

  private func irFR(_ ir: [PrcFmt], fftSize: Int) -> FrequencyResponse {
    var padded = ir
    if padded.count < fftSize {
      padded.append(contentsOf: [PrcFmt](repeating: 0, count: fftSize - padded.count))
    } else if padded.count > fftSize {
      padded.removeLast(padded.count - fftSize)
    }
    return FrequencyResponse.from(
      impulseResponse: ImpulseResponse(samples: padded, sampleRate: sampleRate),
      fftSize: fftSize)
  }

  /// Magnitude of the source biquad chain at frequency `f`.
  private func chainMagDB(_ chain: [BiquadParameters], at f: Double) -> Double {
    var dB = 0.0
    for p in chain {
      guard let c = BiquadCoefficients.compute(parameters: p, sampleRate: sampleRate) else {
        continue
      }
      dB += c.gainDB(atFreqHz: f, sampleRate: sampleRate)
    }
    return dB
  }

  /// Empty chain + 0 dB preamp → IR should be a Kronecker delta.
  @Test func MinPhaseFlatIsDelta() {
    let opts = FIRDesign.Options(fftSize: 1024, outputLength: 1024, preampDB: 0)
    let ir = FIRDesign.minimumPhase(from: [], sampleRate: sampleRate, options: opts)
    #expect(ir.count == 1024)
    #expect(abs(ir[0] - 1.0) < 1e-6, "min-phase delta peak: ir[0] = \(ir[0]) expected 1.0")
    var maxOff: Double = 0
    for i in 1..<ir.count { maxOff = max(maxOff, abs(ir[i])) }
    #expect(maxOff < 1e-6, "min-phase delta off-peak max = \(maxOff) > 1e-6")
  }

  @Test func LinearPhaseFlatIsCenteredDelta() {
    let opts = FIRDesign.Options(fftSize: 1024, outputLength: 1024, preampDB: 0)
    let ir = FIRDesign.linearPhase(from: [], sampleRate: sampleRate, options: opts)
    #expect(ir.count == 1024)
    let center = 1024 / 2
    #expect(
      abs(ir[center] - 1.0) < 1e-6,
      "linear-phase delta peak: ir[\(center)] = \(ir[center]) expected 1.0")
    var maxOff: Double = 0
    for i in 0..<ir.count where i != center { maxOff = max(maxOff, abs(ir[i])) }
    #expect(maxOff < 1e-6, "linear-phase delta off-peak max = \(maxOff) > 1e-6")
  }

  /// Single peaking biquad → designed IR's magnitude response should
  /// match the analytic biquad response to ~1 dB across the audio band.
  /// Uses 0 dB preamp so absolute values are directly comparable.
  @Test func MinPhaseMagnitudeMatchesSinglePeak() {
    let chain: [BiquadParameters] = [
      .init(type: .peaking, freq: 1000, gain: 6, q: 2.0)
    ]
    let opts = FIRDesign.Options(fftSize: 4096, outputLength: 4096, preampDB: 0)
    let ir = FIRDesign.minimumPhase(from: chain, sampleRate: sampleRate, options: opts)
    let fr = irFR(ir, fftSize: 4096)
    let binHz = Double(sampleRate) / Double(4096)

    var maxErr: Double = 0
    for k in 1..<fr.bins {
      let f = Double(k) * binHz
      if f < 50 || f > 10_000 { continue }
      let truth = chainMagDB(chain, at: f)
      let designed = fr.magnitudeDB(at: k)
      maxErr = max(maxErr, abs(designed - truth))
    }
    #expect(
      maxErr < 1.0,
      "min-phase magnitude err = \(maxErr) dB across [50, 10k] Hz, expected < 1.0")
  }

  @Test func LinearPhaseMagnitudeMatchesMultiPeak() {
    let chain: [BiquadParameters] = [
      .init(type: .peaking, freq: 100, gain: -4, q: 1.5),
      .init(type: .peaking, freq: 1000, gain: 6, q: 2.0),
      .init(type: .peaking, freq: 5000, gain: -3, q: 1.5),
    ]
    let opts = FIRDesign.Options(fftSize: 4096, outputLength: 4096, preampDB: 0)
    let ir = FIRDesign.linearPhase(from: chain, sampleRate: sampleRate, options: opts)
    let fr = irFR(ir, fftSize: 4096)
    let binHz = Double(sampleRate) / Double(4096)

    var maxErr: Double = 0
    for k in 1..<fr.bins {
      let f = Double(k) * binHz
      if f < 50 || f > 10_000 { continue }
      let truth = chainMagDB(chain, at: f)
      let designed = fr.magnitudeDB(at: k)
      maxErr = max(maxErr, abs(designed - truth))
    }
    #expect(
      maxErr < 1.0,
      "linear-phase magnitude err = \(maxErr) dB across [50, 10k] Hz, expected < 1.0")
  }

  /// Min-phase IRs should be causal — most of their energy lives near
  /// n = 0. Test by computing the centroid of |h[n]|² and checking it
  /// is well below the centre of the IR.
  @Test func MinPhaseIsCausal() {
    let chain: [BiquadParameters] = [
      .init(type: .peaking, freq: 100, gain: -4, q: 1.5),
      .init(type: .peaking, freq: 1000, gain: 6, q: 2.0),
    ]
    let opts = FIRDesign.Options(fftSize: 4096, outputLength: 4096, preampDB: 0)
    let ir = FIRDesign.minimumPhase(from: chain, sampleRate: sampleRate, options: opts)
    var totalE = 0.0
    var weightedSum = 0.0
    for i in 0..<ir.count {
      let e = ir[i] * ir[i]
      totalE += e
      weightedSum += Double(i) * e
    }
    let centroid = weightedSum / totalE
    #expect(
      centroid < 200.0,
      "min-phase IR centroid = \(centroid) samples; expected ≪ 4096/2")
  }

  /// Linear-phase IRs must be symmetric around (N-1)/2.
  @Test func LinearPhaseIsSymmetric() {
    let chain: [BiquadParameters] = [
      .init(type: .peaking, freq: 200, gain: -3, q: 1.4),
      .init(type: .peaking, freq: 2_000, gain: 4, q: 2.0),
    ]
    let n = 1024
    let opts = FIRDesign.Options(fftSize: n, outputLength: n, preampDB: 0)
    let ir = FIRDesign.linearPhase(from: chain, sampleRate: sampleRate, options: opts)
    let center = n / 2
    var maxAsym: Double = 0
    for k in 1..<center {
      maxAsym = max(maxAsym, abs(ir[center - k] - ir[center + k]))
    }
    #expect(
      maxAsym < 1e-9,
      "linear-phase IR asymmetry = \(maxAsym); expected ~0")
  }

  /// End-to-end: feed the IR into a `ConvolutionFilter` and verify
  /// the convolution's output magnitude tracks the chain's response.
  @Test func MinPhaseRoundTripThroughConvolutionFilter() {
    let chain: [BiquadParameters] = [
      .init(type: .peaking, freq: 1_000, gain: 6, q: 2.0)
    ]
    let opts = FIRDesign.Options(fftSize: 4096, outputLength: 4096, preampDB: 0)
    let ir = FIRDesign.minimumPhase(from: chain, sampleRate: sampleRate, options: opts)

    // Pump a unit impulse through the convolution filter — the
    // multi-block output should equal the IR over its duration.
    let chunk = 1024
    let filter = ConvolutionFilter(coefficients: ir, chunkSize: chunk)

    var collected: [PrcFmt] = []
    collected.reserveCapacity(ir.count + chunk)
    var buf = [PrcFmt](repeating: 0, count: chunk)
    buf[0] = 1.0
    filter.process(waveform: &buf)
    collected.append(contentsOf: buf)
    for _ in 0..<3 {
      var b = [PrcFmt](repeating: 0, count: chunk)
      filter.process(waveform: &b)
      collected.append(contentsOf: b)
    }

    var maxErr: Double = 0
    for i in 0..<ir.count {
      maxErr = max(maxErr, abs(collected[i] - ir[i]))
    }
    #expect(
      maxErr < 1e-6,
      "ConvolutionFilter round-trip err = \(maxErr); expected < 1e-6")
  }
}
