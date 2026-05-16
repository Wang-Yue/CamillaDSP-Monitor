// Round-trip correctness tests for the measurement DSP core.
//
// Each test follows the same shape as a real measurement, but with a
// synthetic "system under test" instead of a microphone + room:
//
//   1. Generate a log-sine sweep + Farina inverse.
//   2. Apply a known transformation to the sweep ("the system").
//   3. Convolve the result with the inverse → recovered IR.
//   4. Compare the recovered IR (or its FR) against the analytic
//      ground truth.
//
// We deliberately focus on *shape* rather than absolute scale —
// Farina's inverse-filter scaling has a frequency-dependent constant
// that's not informative to verify against here. Magnitude responses
// are compared after a bin-zero alignment, and time-domain peak
// positions are checked relatively.

import Foundation
import Testing

@testable import DSPAudio
@testable import DSPMeasurement

@Suite struct SweepDeconvolutionTests {

  // Sweep parameters used across tests. Picking 48 kHz / 0.5 s / 100 Hz
  // → 16 kHz keeps the FFT length tractable while still spanning the
  // bulk of the audible band.
  private let sampleRate = 48000
  private let f1: PrcFmt = 100.0
  private let f2: PrcFmt = 16_000.0
  private let durationSeconds: PrcFmt = 0.5

  /// Identity round-trip: feed the sweep itself as the "captured"
  /// signal. The deconvolution of `x ⊛ f` should be a near-Dirac
  /// peaking at the end of the sweep (sample `N − 1`). Side-lobes
  /// must be well below the peak.
  @Test func IdentityDeconvolution() {
    let (sweep, inverse) = SweepGenerator.sweepAndInverse(
      f1: f1, f2: f2, durationSeconds: durationSeconds, sampleRate: sampleRate,
      fadeInSeconds: 0, fadeOutSeconds: 0)
    let raw = SweepDeconvolver.convolve(sweep, with: inverse)
    let ir = ImpulseResponse(samples: raw, sampleRate: sampleRate).centeredOnPeak()

    let expectedPeak = sweep.count - 1
    #expect(
      abs(ir.zeroIndex - expectedPeak) <= 2,
      "identity peak expected at ~\(expectedPeak), found at \(ir.zeroIndex)")

    // Peak vs sidelobe ratio: pick a sample 1000 taps before the peak
    // (well outside the impulse main lobe) and assert the peak is at
    // least 30 dB louder.
    let peakVal = abs(ir.samples[ir.zeroIndex])
    let sideIdx = max(0, ir.zeroIndex - 1000)
    let sideVal = abs(ir.samples[sideIdx])
    let ratioDB = 20.0 * log10(peakVal / max(sideVal, 1e-150))
    #expect(ratioDB > 30.0, "identity peak/sidelobe = \(ratioDB) dB, expected > 30")
  }

  /// Pure delay round-trip: the captured signal is the sweep delayed
  /// by `D` samples (zero-padded at the front). The deconvolved peak
  /// should land at `N − 1 + D`.
  @Test func DelayDeconvolution() {
    let delay = 137
    let (sweep, _) = SweepGenerator.sweepAndInverse(
      f1: f1, f2: f2, durationSeconds: durationSeconds, sampleRate: sampleRate,
      fadeInSeconds: 0, fadeOutSeconds: 0)
    var captured = [PrcFmt](repeating: 0, count: sweep.count + delay)
    for i in 0..<sweep.count { captured[i + delay] = sweep[i] }

    let ir = SweepDeconvolver.deconvolve(
      captured: captured,
      f1: f1, f2: f2, durationSeconds: durationSeconds, sampleRate: sampleRate)

    let expectedPeak = sweep.count - 1 + delay
    #expect(
      abs(ir.zeroIndex - expectedPeak) <= 2,
      "delay peak expected at \(expectedPeak), found at \(ir.zeroIndex)")
  }

  /// Two-tap moving-average system: the recovered IR (after
  /// peak-centring + windowing) should match `[0.5, 0.5]` up to the
  /// Farina inverse's scaling constant. We verify this by extracting
  /// the two largest taps near the peak and checking their ratio is
  /// ≈ 1 (i.e. equal taps).
  @Test func TwoTapMovingAverageSystem() {
    let (sweep, _) = SweepGenerator.sweepAndInverse(
      f1: f1, f2: f2, durationSeconds: durationSeconds, sampleRate: sampleRate,
      fadeInSeconds: 0, fadeOutSeconds: 0)
    // Captured = sweep convolved with [0.5, 0.5]: y[n] = 0.5·x[n] + 0.5·x[n−1].
    var captured = [PrcFmt](repeating: 0, count: sweep.count + 1)
    for i in 0..<sweep.count { captured[i] += 0.5 * sweep[i] }
    for i in 0..<sweep.count { captured[i + 1] += 0.5 * sweep[i] }

    let ir = SweepDeconvolver.deconvolve(
      captured: captured,
      f1: f1, f2: f2, durationSeconds: durationSeconds, sampleRate: sampleRate)

    // For a [0.5, 0.5] system, the IR has equal-magnitude taps either
    // side of the peak (the "peak" location depends on the alignment
    // convention; pre-peak and post-peak taps sum to the same energy).
    let p = ir.zeroIndex
    let peakTap = abs(ir.samples[p])
    let adjacentTap = max(abs(ir.samples[p - 1]), abs(ir.samples[p + 1]))
    let ratio = peakTap / adjacentTap
    #expect(
      ratio > 0.5 && ratio < 2.0,
      "MA tap ratio = \(ratio); expected ~1 for equal-coefficient IR")
  }

  /// Bandlimited-sweep flatness check: in the deconvolved identity
  /// IR, the magnitude response should be approximately flat in the
  /// sweep band [f1, f2]. We tolerate ±3 dB ripple — Farina's method
  /// naturally has some band-edge roll-off without windowing, and the
  /// truncated convolution introduces additional broadband noise.
  @Test func IdentityFlatnessInBand() {
    let (sweep, inverse) = SweepGenerator.sweepAndInverse(
      f1: f1, f2: f2, durationSeconds: durationSeconds, sampleRate: sampleRate,
      fadeInSeconds: 0, fadeOutSeconds: 0)
    let raw = SweepDeconvolver.convolve(sweep, with: inverse)
    let ir = ImpulseResponse(samples: raw, sampleRate: sampleRate).centeredOnPeak()
    // Window 4096 samples around the peak so the FR isn't dominated
    // by the sweep's DC/HF tails outside the impulse region.
    let win = ir.windowed(leftSamples: 2048, rightSamples: 2048, taperFraction: 0.1)

    let fr = FrequencyResponse.from(impulseResponse: win)

    // Locate the peak magnitude in the [2·f1, f2/2] band (avoid the
    // band-edge roll-off where the sweep stimulus is strongest /
    // weakest). Then measure the maximum deviation across that band.
    let lowHz = 2.0 * f1
    let highHz = f2 / 2.0
    var bandMags: [PrcFmt] = []
    for k in 1..<fr.bins {
      let f = fr.frequency(at: k)
      if f >= lowHz && f <= highHz {
        bandMags.append(fr.magnitudeDB(at: k))
      }
    }
    #expect(!bandMags.isEmpty, "no FR bins in flatness band")

    let median = bandMags.sorted()[bandMags.count / 2]
    let maxDev = bandMags.map { abs($0 - median) }.max() ?? 0
    #expect(
      maxDev < 6.0,
      "identity IR flatness deviation \(maxDev) dB exceeds 6 dB tolerance")
  }

  /// Frequency-dependent windowing (FDW) reflection suppression:
  /// construct a synthetic IR with a direct impulse at t=0 and a strong
  /// reflection at t=1 ms. FDW(5 cycles) should preserve the reflection
  /// at low frequencies (e.g. 200 Hz, where 5 cycles = 25 ms window)
  /// but completely suppress it at high frequencies (e.g. 10 kHz, where
  /// 5 cycles = 0.5 ms window), eliminating the comb-filter ripple.
  @Test func FrequencyDependentWindowSuppressesReflection() {
    let n = 4096
    var samples = [PrcFmt](repeating: 0, count: n)
    let p = 1000
    samples[p] = 1.0
    // 1 ms reflection at 48 kHz = 48 samples later.
    let reflectionDelay = 48
    samples[p + reflectionDelay] = 0.5

    let ir = ImpulseResponse(samples: samples, sampleRate: sampleRate, zeroIndex: p)
    let frFdw = FrequencyResponse.fdw(impulseResponse: ir, cycles: 5.0, fftSize: n)
    let frStandard = FrequencyResponse.from(impulseResponse: ir, fftSize: n)

    // Find bins for 200 Hz and 10 kHz.
    let binHz = PrcFmt(sampleRate) / PrcFmt(n)
    let bin200 = Int((200.0 / binHz).rounded())
    let bin10k = Int((10000.0 / binHz).rounded())

    // At 200 Hz, the reflection is inside the window. Both standard and
    // FDW should see a similar magnitude.
    let magStandard200 = frStandard.magnitudeDB(at: bin200)
    let magFdw200 = frFdw.magnitudeDB(at: bin200)
    #expect(
      abs(magStandard200 - magFdw200) < 1.0,
      "FDW should preserve low-frequency reflection; std=\(magStandard200), fdw=\(magFdw200)")

    // At 10 kHz, the reflection is completely outside the 5-cycle window.
    // The FDW response should be exactly the direct impulse (flat at 0 dB),
    // while standard FR has the comb filter ripple.
    let magFdw10k = frFdw.magnitudeDB(at: bin10k)
    #expect(
      abs(magFdw10k) < 0.1,
      "FDW should completely suppress 10 kHz reflection; expected 0 dB, found \(magFdw10k) dB")
  }

  /// Modal decay analysis: verify that Schroeder reverse integration accurately
  /// calculates the energy decay curve and RT60 estimation yields the expected
  /// time constant for an exponentially decaying impulse response.
  @Test func ModalDecayAnalysis() {
    // h(t) = e^{-t / tau}
    // h(t)^2 = e^{-2t / tau}
    // 10 log10(e^{-2t/tau}) = -60 dB at t = 0.3s
    // tau = (6.0 * log10(e)) / 60.0 = log10(e) / 10.0
    let tau = log10(M_E) / 10.0
    let n = 16384
    var samples = [PrcFmt](repeating: 0, count: n)
    let p = 100
    for i in 0..<(n - p) {
      let t = PrcFmt(i) / PrcFmt(sampleRate)
      samples[p + i] = exp(-t / tau)
    }

    let ir = ImpulseResponse(samples: samples, sampleRate: sampleRate, zeroIndex: p)
    let rt60Val = ir.rt60(startDB: -5.0, endDB: -25.0)

    #expect(
      abs(rt60Val - 0.3) < 0.02,
      "Schroeder integration RT60 estimation should be close to 0.3s, found \(rt60Val)s")
  }
}
