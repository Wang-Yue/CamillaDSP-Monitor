// Biquad filter tests

import Foundation
import Testing

@testable import DSPAudio
@testable import DSPConfig
@testable import DSPFilters

// MARK: - Helpers (match Rust helpers exactly)

func gainAndPhase(coeffs: BiquadCoefficients, f: Double, fs: Double) -> (
  gainDB: Double, phaseDeg: Double
) {
  let w = 2.0 * Double.pi * f / fs
  let cosW = cos(w)
  let sinW = sin(w)
  let cos2W = cos(2.0 * w)
  let sin2W = sin(2.0 * w)
  let numRe = coeffs.b0 + coeffs.b1 * cosW + coeffs.b2 * cos2W
  let numIm = -coeffs.b1 * sinW - coeffs.b2 * sin2W
  let denRe = 1.0 + coeffs.a1 * cosW + coeffs.a2 * cos2W
  let denIm = -coeffs.a1 * sinW - coeffs.a2 * sin2W
  let denMag2 = denRe * denRe + denIm * denIm
  let hRe = (numRe * denRe + numIm * denIm) / denMag2
  let hIm = (numIm * denRe - numRe * denIm) / denMag2
  let mag = sqrt(hRe * hRe + hIm * hIm)
  let gainDB = 20.0 * log10(max(mag, 1e-150))
  let phaseDeg = atan2(hIm, hRe) * 180.0 / Double.pi
  return (gainDB, phaseDeg)
}

func isClose(_ left: Double, _ right: Double, _ maxdiff: Double) -> Bool {
  abs(left - right) < maxdiff
}

func isCloseRelative(_ left: Double, _ right: Double, _ maxdiff: Double) -> Bool {
  abs(left / right - 1.0) < maxdiff
}

// MARK: - BiquadTests

@Suite struct BiquadTests {

  private let fs: Double = 44100.0

  private func makeCoeffs(
    type: BiquadType,
    freq: Double? = nil, q: Double? = nil, gain: Double? = nil,
    slope: Double? = nil, bandwidth: Double? = nil,
    a1: Double? = nil, a2: Double? = nil, b0: Double? = nil, b1: Double? = nil, b2: Double? = nil,
    freqNotch: Double? = nil, freqPole: Double? = nil, normalizeAtDc: Bool? = nil,
    freqAct: Double? = nil, qAct: Double? = nil, freqTarget: Double? = nil, qTarget: Double? = nil,
    sampleRate: Int? = nil
  ) throws -> BiquadCoefficients {
    let params = BiquadParameters(
      type: type, freq: freq, gain: gain, q: q, bandwidth: bandwidth, slope: slope,
      a1: a1, a2: a2, b0: b0, b1: b1, b2: b2,
      freqNotch: freqNotch, freqPole: freqPole, normalizeAtDc: normalizeAtDc,
      freqAct: freqAct, qAct: qAct, freqTarget: freqTarget, qTarget: qTarget
    )
    let sr = sampleRate ?? Int(fs)
    let config = FilterConfig.biquad(params)
    try config.validate()
    try params.validate(sampleRate: sr)
    return try BiquadFilter.computeCoefficients(params, sampleRate: sr)
  }

  // Rust: check_result — impulse response of LP 10kHz Q=0.5
  @Test func ImpulseResponse() throws {
    let coeffs = try makeCoeffs(type: .lowpass, freq: 10000.0, q: 0.5)
    let filter = BiquadFilter(coefficients: coeffs)
    var wave: [PrcFmt] = [1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
    let expected: [Double] = [0.215, 0.461, 0.281, 0.039, 0.004, 0.0, 0.0, 0.0]
    filter.process(waveform: &wave)
    for (i, exp) in expected.enumerated() {
      #expect(isClose(wave[i], exp, 1e-3))
    }
  }

  // Rust: make_lowpass — Butterworth LP 100 Hz
  @Test func Lowpass() throws {
    let coeffs = try makeCoeffs(type: .lowpass, freq: 100.0, q: 1.0 / sqrt(2.0))
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 400.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 10.0, fs: fs)
    #expect(isClose(gf0, -3.0, 0.1))
    #expect(isClose(glf, 0.0, 0.1))
    #expect(isClose(ghf, -24.0, 0.2))
  }

  // Rust: make_highpass
  @Test func Highpass() throws {
    let coeffs = try makeCoeffs(type: .highpass, freq: 100.0, q: 1.0 / sqrt(2.0))
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 400.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 25.0, fs: fs)
    #expect(isClose(gf0, -3.0, 0.1))
    #expect(isClose(glf, -24.0, 0.2))
    #expect(isClose(ghf, 0.0, 0.1))
  }

  // Rust: make_lowpass_fo
  @Test func LowpassFO() throws {
    let coeffs = try makeCoeffs(type: .lowpassFO, freq: 100.0)
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 400.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 10.0, fs: fs)
    #expect(isClose(gf0, -3.0, 0.1))
    #expect(isClose(glf, 0.0, 0.1))
    #expect(isClose(ghf, -12.3, 0.1))
  }

  // Rust: make_highpass_fo
  @Test func HighpassFO() throws {
    let coeffs = try makeCoeffs(type: .highpassFO, freq: 100.0)
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 800.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 25.0, fs: fs)
    #expect(isClose(gf0, -3.0, 0.1))
    #expect(isClose(glf, -12.3, 0.1))
    #expect(isClose(ghf, 0.0, 0.1))
  }

  // Rust: make_peaking
  @Test func Peaking() throws {
    let coeffs = try makeCoeffs(type: .peaking, freq: 100.0, q: 3.0, gain: 7.0)
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 400.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 25.0, fs: fs)
    #expect(isClose(gf0, 7.0, 0.1))
    #expect(isClose(glf, 0.0, 0.1))
    #expect(isClose(ghf, 0.0, 0.1))
  }

  // Rust: make_bandpass
  @Test func Bandpass() throws {
    let coeffs = try makeCoeffs(type: .bandpass, freq: 100.0, q: 1.0)
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 400.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 25.0, fs: fs)
    #expect(isClose(gf0, 0.0, 0.1))
    #expect(isClose(glf, -12.0, 0.3))
    #expect(isClose(ghf, -12.0, 0.3))
  }

  // Rust: make_notch
  @Test func Notch() throws {
    let coeffs = try makeCoeffs(type: .notch, freq: 100.0, q: 3.0)
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 400.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 25.0, fs: fs)
    #expect(gf0 < -40.0)
    #expect(isClose(glf, 0.0, 0.1))
    #expect(isClose(ghf, 0.0, 0.1))
  }

  // Rust: make_allpass
  @Test func Allpass() throws {
    let coeffs = try makeCoeffs(type: .allpass, freq: 100.0, q: 3.0)
    let (gf0, pf0) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, phf) = gainAndPhase(coeffs: coeffs, f: 10000.0, fs: fs)
    let (glf, plf) = gainAndPhase(coeffs: coeffs, f: 1.0, fs: fs)
    #expect(isClose(gf0, 0.0, 0.1))
    #expect(isClose(glf, 0.0, 0.1))
    #expect(isClose(ghf, 0.0, 0.1))
    #expect(isClose(abs(pf0), 180.0, 0.5))
    #expect(isClose(plf, 0.0, 0.5))
    #expect(isClose(phf, 0.0, 0.5))
  }

  // Rust: make_allpass_fo
  @Test func AllpassFO() throws {
    let coeffs = try makeCoeffs(type: .allpassFO, freq: 100.0)
    let (gf0, pf0) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, phf) = gainAndPhase(coeffs: coeffs, f: 10000.0, fs: fs)
    let (glf, plf) = gainAndPhase(coeffs: coeffs, f: 1.0, fs: fs)
    #expect(isClose(gf0, 0.0, 0.1))
    #expect(isClose(glf, 0.0, 0.1))
    #expect(isClose(ghf, 0.0, 0.1))
    #expect(isClose(abs(pf0), 90.0, 0.5))
    #expect(isClose(plf, 0.0, 2.0))
    #expect(isClose(abs(phf), 180.0, 2.0))
  }

  // Rust: make_highshelf — slope=6, gain=-24, freq=100
  @Test func Highshelf() throws {
    let coeffs = try makeCoeffs(type: .highshelf, freq: 100.0, gain: -24.0, slope: 6.0)
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (gf0h, _) = gainAndPhase(coeffs: coeffs, f: 200.0, fs: fs)
    let (gf0l, _) = gainAndPhase(coeffs: coeffs, f: 50.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 10000.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 1.0, fs: fs)
    #expect(isClose(gf0, -12.0, 0.1))
    #expect(isClose(gf0h, -18.0, 1.0))
    #expect(isClose(gf0l, -6.0, 1.0))
    #expect(isClose(glf, 0.0, 0.1))
    #expect(isClose(ghf, -24.0, 0.1))
  }

  // Rust: make_lowshelf — slope=6, gain=-24, freq=100
  @Test func Lowshelf() throws {
    let coeffs = try makeCoeffs(type: .lowshelf, freq: 100.0, gain: -24.0, slope: 6.0)
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (gf0h, _) = gainAndPhase(coeffs: coeffs, f: 200.0, fs: fs)
    let (gf0l, _) = gainAndPhase(coeffs: coeffs, f: 50.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 10000.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 1.0, fs: fs)
    #expect(isClose(gf0, -12.0, 0.1))
    #expect(isClose(gf0h, -6.0, 1.0))
    #expect(isClose(gf0l, -18.0, 1.0))
    #expect(isClose(glf, -24.0, 0.1))
    #expect(isClose(ghf, 0.0, 0.1))
  }

  // Rust: lowshelf_slope_vs_q — slope=12 == Q=1/√2 (relative tolerance 0.1%)
  @Test func LowshelfSlopeVsQ() throws {
    let cS = try makeCoeffs(type: .lowshelf, freq: 100.0, gain: -24.0, slope: 12.0)
    let cQ = try makeCoeffs(type: .lowshelf, freq: 100.0, q: 1.0 / sqrt(2.0), gain: -24.0)
    #expect(isCloseRelative(cS.a1, cQ.a1, 0.001))
    #expect(isCloseRelative(cS.a2, cQ.a2, 0.001))
    #expect(isCloseRelative(cS.b0, cQ.b0, 0.001))
    #expect(isCloseRelative(cS.b1, cQ.b1, 0.001))
    #expect(isCloseRelative(cS.b2, cQ.b2, 0.001))
  }

  // Rust: highshelf_slope_vs_q
  @Test func HighshelfSlopeVsQ() throws {
    let cS = try makeCoeffs(type: .highshelf, freq: 100.0, gain: -24.0, slope: 12.0)
    let cQ = try makeCoeffs(type: .highshelf, freq: 100.0, q: 1.0 / sqrt(2.0), gain: -24.0)
    #expect(isCloseRelative(cS.a1, cQ.a1, 0.001))
    #expect(isCloseRelative(cS.a2, cQ.a2, 0.001))
    #expect(isCloseRelative(cS.b0, cQ.b0, 0.001))
    #expect(isCloseRelative(cS.b1, cQ.b1, 0.001))
    #expect(isCloseRelative(cS.b2, cQ.b2, 0.001))
  }

  // Rust: bandpass_bw_vs_q — bandwidth=1 oct == Q=√2
  @Test func BandpassBWvsQ() throws {
    let cBW = try makeCoeffs(type: .bandpass, freq: 100.0, bandwidth: 1.0)
    let cQ = try makeCoeffs(type: .bandpass, freq: 100.0, q: sqrt(2.0))
    #expect(isCloseRelative(cBW.a1, cQ.a1, 0.001))
    #expect(isCloseRelative(cBW.a2, cQ.a2, 0.001))
    #expect(isCloseRelative(cBW.b0, cQ.b0, 0.001))
    #expect(cBW.b1 == 0.0)
    #expect(cQ.b1 == 0.0)
    #expect(isCloseRelative(cBW.b2, cQ.b2, 0.001))
  }

  // Rust: notch_bw_vs_q
  @Test func NotchBWvsQ() throws {
    let cBW = try makeCoeffs(type: .notch, freq: 100.0, bandwidth: 1.0)
    let cQ = try makeCoeffs(type: .notch, freq: 100.0, q: sqrt(2.0))
    #expect(isCloseRelative(cBW.a1, cQ.a1, 0.001))
    #expect(isCloseRelative(cBW.a2, cQ.a2, 0.001))
    #expect(isCloseRelative(cBW.b0, cQ.b0, 0.001))
    #expect(isCloseRelative(cBW.b1, cQ.b1, 0.001))
    #expect(isCloseRelative(cBW.b2, cQ.b2, 0.001))
  }

  // Rust: allpass_bw_vs_q
  @Test func AllpassBWvsQ() throws {
    let cBW = try makeCoeffs(type: .allpass, freq: 100.0, bandwidth: 1.0)
    let cQ = try makeCoeffs(type: .allpass, freq: 100.0, q: sqrt(2.0))
    #expect(isCloseRelative(cBW.a1, cQ.a1, 0.001))
    #expect(isCloseRelative(cBW.a2, cQ.a2, 0.001))
    #expect(isCloseRelative(cBW.b0, cQ.b0, 0.001))
    #expect(isCloseRelative(cBW.b1, cQ.b1, 0.001))
    #expect(isCloseRelative(cBW.b2, cQ.b2, 0.001))
  }

  // Rust: make_highshelf_fo — gain=-12, freq=100
  @Test func HighshelfFO() throws {
    let coeffs = try makeCoeffs(type: .highshelfFO, freq: 100.0, gain: -12.0)
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 10000.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 1.0, fs: fs)
    #expect(isClose(gf0, -6.0, 0.1))
    #expect(isClose(glf, 0.0, 0.1))
    #expect(isClose(ghf, -12.0, 0.1))
  }

  // Rust: make_lowshelf_fo — gain=-12, freq=100
  @Test func LowshelfFO() throws {
    let coeffs = try makeCoeffs(type: .lowshelfFO, freq: 100.0, gain: -12.0)
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 10000.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 1.0, fs: fs)
    #expect(isClose(gf0, -6.0, 0.1))
    #expect(isClose(glf, -12.0, 0.1))
    #expect(isClose(ghf, 0.0, 0.1))
  }

  // Rust: check_freq_q — validation tests
  @Test func ValidateFreqQ() throws {
    let fs48 = 48000
    // OK: freq=1000, Q=2
    _ = try makeCoeffs(type: .peaking, freq: 1000.0, q: 2.0, gain: 1.23, sampleRate: fs48)
    // Bad: Q=0
    do {
      _ = try makeCoeffs(type: .peaking, freq: 1000.0, q: 0.0, gain: 1.23, sampleRate: fs48)
      Issue.record("Expected error to be thrown")
    } catch {
      // expected exception
    }
    // Bad: freq > Nyquist
    do {
      _ = try makeCoeffs(type: .peaking, freq: 25000.0, q: 1.0, gain: 1.23, sampleRate: fs48)
      Issue.record("Expected error to be thrown")
    } catch {
      // expected exception
    }
    // Bad: freq = 0
    do {
      _ = try makeCoeffs(type: .peaking, freq: 0.0, q: 1.0, gain: 1.23, sampleRate: fs48)
      Issue.record("Expected error to be thrown")
    } catch {
      // expected exception
    }
  }

  // Rust: check_slope — validation tests
  @Test func ValidateSlope() throws {
    let fs48 = 48000
    // OK: slope=5
    _ = try makeCoeffs(type: .highshelf, freq: 1000.0, gain: 1.23, slope: 5.0, sampleRate: fs48)
    // Bad: slope=0
    do {
      _ = try makeCoeffs(type: .highshelf, freq: 1000.0, gain: 1.23, slope: 0.0, sampleRate: fs48)
      Issue.record("Expected error to be thrown")
    } catch {
      // expected exception
    }
    // Bad: slope=15 (> 12)
    do {
      _ = try makeCoeffs(type: .highshelf, freq: 1000.0, gain: 1.23, slope: 15.0, sampleRate: fs48)
      Issue.record("Expected error to be thrown")
    } catch {
      // expected exception
    }
  }

  @Test func FreeBiquad() throws {
    let coeffs = try makeCoeffs(
      type: .free,
      a1: -0.5, a2: 0.1, b0: 0.25, b1: 0.5, b2: 0.25
    )
    #expect(coeffs.b0 == 0.25)
    #expect(coeffs.b1 == 0.5)
    #expect(coeffs.b2 == 0.25)
    #expect(coeffs.a1 == -0.5)
    #expect(coeffs.a2 == 0.1)
  }

  @Test func GeneralNotchHP() throws {
    let coeffs = try makeCoeffs(
      type: .generalNotch,
      q: 1.0,
      freqNotch: 1000.0,
      freqPole: 2000.0,
      normalizeAtDc: false
    )
    let (gainFp, _) = gainAndPhase(coeffs: coeffs, f: 1000.0, fs: fs)
    let (gainHf, _) = gainAndPhase(coeffs: coeffs, f: 20000.0, fs: fs)
    let (gainLf, _) = gainAndPhase(coeffs: coeffs, f: 1.0, fs: fs)
    #expect(gainFp < -40.0)
    #expect(isClose(gainLf, -12.1, 0.1))
    #expect(isClose(gainHf, 0.0, 0.1))
  }

  @Test func GeneralNotchLP() throws {
    let coeffs = try makeCoeffs(
      type: .generalNotch,
      q: 1.0,
      freqNotch: 1000.0,
      freqPole: 500.0,
      normalizeAtDc: true
    )
    let (gainFp, _) = gainAndPhase(coeffs: coeffs, f: 1000.0, fs: fs)
    let (gainHf, _) = gainAndPhase(coeffs: coeffs, f: 20000.0, fs: fs)
    let (gainLf, _) = gainAndPhase(coeffs: coeffs, f: 1.0, fs: fs)
    #expect(gainFp < -40.0)
    #expect(isClose(gainLf, 0.0, 0.1))
    #expect(isClose(gainHf, -12.1, 0.1))
  }

  @Test func LinkwitzTransform() throws {
    let coeffs = try makeCoeffs(
      type: .linkwitzTransform,
      freqAct: 100.0, qAct: 1.2,
      freqTarget: 25.0, qTarget: 0.7
    )
    let (gain10, _) = gainAndPhase(coeffs: coeffs, f: 10.0, fs: fs)
    let (gain87, _) = gainAndPhase(coeffs: coeffs, f: 87.0, fs: fs)
    let (gain123, _) = gainAndPhase(coeffs: coeffs, f: 123.0, fs: fs)
    let (gainHf, _) = gainAndPhase(coeffs: coeffs, f: 10000.0, fs: fs)
    #expect(isClose(gain10, 23.9, 0.1))
    #expect(isClose(gain87, 0.0, 0.1))
    #expect(isClose(gain123, -2.4, 0.1))
    #expect(isClose(gainHf, 0.0, 0.1))
  }
}
