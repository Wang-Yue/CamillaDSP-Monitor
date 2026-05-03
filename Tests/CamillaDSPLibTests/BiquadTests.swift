// CamillaDSP-Swift: Biquad filter tests — exact match of Rust CamillaDSP test suite
// Every test mirrors a #[test] function from src/filters/biquad.rs

import XCTest

@testable import CamillaDSPLib

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

final class BiquadTests: XCTestCase {

  private let fs: Double = 44100.0

  private func makeCoeffs(
    type: BiquadType,
    freq: Double? = nil, q: Double? = nil, gain: Double? = nil,
    slope: Double? = nil, bandwidth: Double? = nil,
    freqNotch: Double? = nil, freqPole: Double? = nil, normalizeAtDc: Bool? = nil,
    freqAct: Double? = nil, qAct: Double? = nil,
    freqTarget: Double? = nil, qTarget: Double? = nil,
    sampleRate: Int? = nil
  ) throws -> BiquadCoefficients {
    var params = FilterParameters()
    params.subtype = type.rawValue
    params.freq = freq
    params.q = q
    params.gain = gain
    params.slope = slope
    params.bandwidth = bandwidth
    params.freqNotch = freqNotch
    params.freqPole = freqPole
    params.normalizeAtDc = normalizeAtDc
    params.freqAct = freqAct
    params.qAct = qAct
    params.freqTarget = freqTarget
    params.qTarget = qTarget
    let sr = sampleRate ?? Int(fs)
    let config = FilterConfig(type: .biquad, parameters: params)
    try FilterValidator.validate(config, sampleRate: sr)
    return try BiquadFilter.computeCoefficients(params, sampleRate: sr)
  }

  // Rust: check_result — impulse response of LP 10kHz Q=0.5
  func testImpulseResponse() throws {
    let coeffs = try makeCoeffs(type: .lowpass, freq: 10000.0, q: 0.5)
    let filter = BiquadFilter(name: "test", coefficients: coeffs, sampleRate: 44100)
    var wave: [PrcFmt] = [1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
    let expected: [Double] = [0.215, 0.461, 0.281, 0.039, 0.004, 0.0, 0.0, 0.0]
    try filter.process(waveform: &wave)
    for (i, exp) in expected.enumerated() {
      XCTAssert(isClose(wave[i], exp, 1e-3), "IR[\(i)] = \(wave[i]), expected \(exp)")
    }
  }

  // Rust: make_lowpass — Butterworth LP 100 Hz
  func testLowpass() throws {
    let coeffs = try makeCoeffs(type: .lowpass, freq: 100.0, q: 1.0 / sqrt(2.0))
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 400.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 10.0, fs: fs)
    XCTAssert(isClose(gf0, -3.0, 0.1), "LP f0=\(gf0)")
    XCTAssert(isClose(glf, 0.0, 0.1), "LP lf=\(glf)")
    XCTAssert(isClose(ghf, -24.0, 0.2), "LP hf=\(ghf)")
  }

  // Rust: make_highpass
  func testHighpass() throws {
    let coeffs = try makeCoeffs(type: .highpass, freq: 100.0, q: 1.0 / sqrt(2.0))
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 400.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 25.0, fs: fs)
    XCTAssert(isClose(gf0, -3.0, 0.1), "HP f0=\(gf0)")
    XCTAssert(isClose(glf, -24.0, 0.2), "HP lf=\(glf)")
    XCTAssert(isClose(ghf, 0.0, 0.1), "HP hf=\(ghf)")
  }

  // Rust: make_lowpass_fo
  func testLowpassFO() throws {
    let coeffs = try makeCoeffs(type: .lowpassFO, freq: 100.0)
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 400.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 10.0, fs: fs)
    XCTAssert(isClose(gf0, -3.0, 0.1), "LPFO f0=\(gf0)")
    XCTAssert(isClose(glf, 0.0, 0.1), "LPFO lf=\(glf)")
    XCTAssert(isClose(ghf, -12.3, 0.1), "LPFO hf=\(ghf)")
  }

  // Rust: make_highpass_fo
  func testHighpassFO() throws {
    let coeffs = try makeCoeffs(type: .highpassFO, freq: 100.0)
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 800.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 25.0, fs: fs)
    XCTAssert(isClose(gf0, -3.0, 0.1), "HPFO f0=\(gf0)")
    XCTAssert(isClose(glf, -12.3, 0.1), "HPFO lf=\(glf)")
    XCTAssert(isClose(ghf, 0.0, 0.1), "HPFO hf=\(ghf)")
  }

  // Rust: make_peaking
  func testPeaking() throws {
    let coeffs = try makeCoeffs(type: .peaking, freq: 100.0, q: 3.0, gain: 7.0)
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 400.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 25.0, fs: fs)
    XCTAssert(isClose(gf0, 7.0, 0.1), "Peak f0=\(gf0)")
    XCTAssert(isClose(glf, 0.0, 0.1), "Peak lf=\(glf)")
    XCTAssert(isClose(ghf, 0.0, 0.1), "Peak hf=\(ghf)")
  }

  // Rust: make_bandpass
  func testBandpass() throws {
    let coeffs = try makeCoeffs(type: .bandpass, freq: 100.0, q: 1.0)
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 400.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 25.0, fs: fs)
    XCTAssert(isClose(gf0, 0.0, 0.1), "BP f0=\(gf0)")
    XCTAssert(isClose(glf, -12.0, 0.3), "BP lf=\(glf)")
    XCTAssert(isClose(ghf, -12.0, 0.3), "BP hf=\(ghf)")
  }

  // Rust: make_notch
  func testNotch() throws {
    let coeffs = try makeCoeffs(type: .notch, freq: 100.0, q: 3.0)
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 400.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 25.0, fs: fs)
    XCTAssert(gf0 < -40.0, "Notch f0=\(gf0) should be < -40")
    XCTAssert(isClose(glf, 0.0, 0.1), "Notch lf=\(glf)")
    XCTAssert(isClose(ghf, 0.0, 0.1), "Notch hf=\(ghf)")
  }

  // Rust: make_generalnotch_hp — pole above zero, not normalised at DC
  func testGeneralNotchHP() throws {
    let coeffs = try makeCoeffs(
      type: .generalNotch, q: 1.0,
      freqNotch: 1000.0, freqPole: 2000.0, normalizeAtDc: false)
    let (gZero, _) = gainAndPhase(coeffs: coeffs, f: 1000.0, fs: fs)
    let (gHF, _) = gainAndPhase(coeffs: coeffs, f: 20000.0, fs: fs)
    let (gLF, _) = gainAndPhase(coeffs: coeffs, f: 1.0, fs: fs)
    XCTAssert(gZero < -40.0, "GN_HP zero=\(gZero)")
    XCTAssert(isClose(gLF, -12.1, 0.1), "GN_HP lf=\(gLF)")
    XCTAssert(isClose(gHF, 0.0, 0.1), "GN_HP hf=\(gHF)")
  }

  // Rust: make_generalnotch_lp — zero above pole, normalised at DC
  func testGeneralNotchLP() throws {
    let coeffs = try makeCoeffs(
      type: .generalNotch, q: 1.0,
      freqNotch: 1000.0, freqPole: 500.0, normalizeAtDc: true)
    let (gZero, _) = gainAndPhase(coeffs: coeffs, f: 1000.0, fs: fs)
    let (gHF, _) = gainAndPhase(coeffs: coeffs, f: 20000.0, fs: fs)
    let (gLF, _) = gainAndPhase(coeffs: coeffs, f: 1.0, fs: fs)
    XCTAssert(gZero < -40.0, "GN_LP zero=\(gZero)")
    XCTAssert(isClose(gLF, 0.0, 0.1), "GN_LP lf=\(gLF)")
    XCTAssert(isClose(gHF, -12.1, 0.1), "GN_LP hf=\(gHF)")
  }

  // Rust: make_allpass
  func testAllpass() throws {
    let coeffs = try makeCoeffs(type: .allpass, freq: 100.0, q: 3.0)
    let (gf0, pf0) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, phf) = gainAndPhase(coeffs: coeffs, f: 10000.0, fs: fs)
    let (glf, plf) = gainAndPhase(coeffs: coeffs, f: 1.0, fs: fs)
    XCTAssert(isClose(gf0, 0.0, 0.1), "AP gain f0=\(gf0)")
    XCTAssert(isClose(glf, 0.0, 0.1), "AP gain lf=\(glf)")
    XCTAssert(isClose(ghf, 0.0, 0.1), "AP gain hf=\(ghf)")
    XCTAssert(isClose(abs(pf0), 180.0, 0.5), "AP phase f0=\(pf0)")
    XCTAssert(isClose(plf, 0.0, 0.5), "AP phase lf=\(plf)")
    XCTAssert(isClose(phf, 0.0, 0.5), "AP phase hf=\(phf)")
  }

  // Rust: make_allpass_fo
  func testAllpassFO() throws {
    let coeffs = try makeCoeffs(type: .allpassFO, freq: 100.0)
    let (gf0, pf0) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, phf) = gainAndPhase(coeffs: coeffs, f: 10000.0, fs: fs)
    let (glf, plf) = gainAndPhase(coeffs: coeffs, f: 1.0, fs: fs)
    XCTAssert(isClose(gf0, 0.0, 0.1), "APFO gain f0=\(gf0)")
    XCTAssert(isClose(glf, 0.0, 0.1), "APFO gain lf=\(glf)")
    XCTAssert(isClose(ghf, 0.0, 0.1), "APFO gain hf=\(ghf)")
    XCTAssert(isClose(abs(pf0), 90.0, 0.5), "APFO phase f0=\(pf0)")
    XCTAssert(isClose(plf, 0.0, 2.0), "APFO phase lf=\(plf)")
    XCTAssert(isClose(abs(phf), 180.0, 2.0), "APFO phase hf=\(phf)")
  }

  // Rust: make_highshelf — slope=6, gain=-24, freq=100
  func testHighshelf() throws {
    let coeffs = try makeCoeffs(type: .highshelf, freq: 100.0, gain: -24.0, slope: 6.0)
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (gf0h, _) = gainAndPhase(coeffs: coeffs, f: 200.0, fs: fs)
    let (gf0l, _) = gainAndPhase(coeffs: coeffs, f: 50.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 10000.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 1.0, fs: fs)
    XCTAssert(isClose(gf0, -12.0, 0.1), "HS f0=\(gf0)")
    XCTAssert(isClose(gf0h, -18.0, 1.0), "HS f0h=\(gf0h)")
    XCTAssert(isClose(gf0l, -6.0, 1.0), "HS f0l=\(gf0l)")
    XCTAssert(isClose(glf, 0.0, 0.1), "HS lf=\(glf)")
    XCTAssert(isClose(ghf, -24.0, 0.1), "HS hf=\(ghf)")
  }

  // Rust: make_lowshelf — slope=6, gain=-24, freq=100
  func testLowshelf() throws {
    let coeffs = try makeCoeffs(type: .lowshelf, freq: 100.0, gain: -24.0, slope: 6.0)
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (gf0h, _) = gainAndPhase(coeffs: coeffs, f: 200.0, fs: fs)
    let (gf0l, _) = gainAndPhase(coeffs: coeffs, f: 50.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 10000.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 1.0, fs: fs)
    XCTAssert(isClose(gf0, -12.0, 0.1), "LS f0=\(gf0)")
    XCTAssert(isClose(gf0h, -6.0, 1.0), "LS f0h=\(gf0h)")
    XCTAssert(isClose(gf0l, -18.0, 1.0), "LS f0l=\(gf0l)")
    XCTAssert(isClose(glf, -24.0, 0.1), "LS lf=\(glf)")
    XCTAssert(isClose(ghf, 0.0, 0.1), "LS hf=\(ghf)")
  }

  // Rust: lowshelf_slope_vs_q — slope=12 == Q=1/√2 (relative tolerance 0.1%)
  func testLowshelfSlopeVsQ() throws {
    let cS = try makeCoeffs(type: .lowshelf, freq: 100.0, gain: -24.0, slope: 12.0)
    let cQ = try makeCoeffs(type: .lowshelf, freq: 100.0, q: 1.0 / sqrt(2.0), gain: -24.0)
    XCTAssert(isCloseRelative(cS.a1, cQ.a1, 0.001), "LS a1")
    XCTAssert(isCloseRelative(cS.a2, cQ.a2, 0.001), "LS a2")
    XCTAssert(isCloseRelative(cS.b0, cQ.b0, 0.001), "LS b0")
    XCTAssert(isCloseRelative(cS.b1, cQ.b1, 0.001), "LS b1")
    XCTAssert(isCloseRelative(cS.b2, cQ.b2, 0.001), "LS b2")
  }

  // Rust: highshelf_slope_vs_q
  func testHighshelfSlopeVsQ() throws {
    let cS = try makeCoeffs(type: .highshelf, freq: 100.0, gain: -24.0, slope: 12.0)
    let cQ = try makeCoeffs(type: .highshelf, freq: 100.0, q: 1.0 / sqrt(2.0), gain: -24.0)
    XCTAssert(isCloseRelative(cS.a1, cQ.a1, 0.001), "HS a1")
    XCTAssert(isCloseRelative(cS.a2, cQ.a2, 0.001), "HS a2")
    XCTAssert(isCloseRelative(cS.b0, cQ.b0, 0.001), "HS b0")
    XCTAssert(isCloseRelative(cS.b1, cQ.b1, 0.001), "HS b1")
    XCTAssert(isCloseRelative(cS.b2, cQ.b2, 0.001), "HS b2")
  }

  // Rust: bandpass_bw_vs_q — bandwidth=1 oct == Q=√2
  func testBandpassBWvsQ() throws {
    let cBW = try makeCoeffs(type: .bandpass, freq: 100.0, bandwidth: 1.0)
    let cQ = try makeCoeffs(type: .bandpass, freq: 100.0, q: sqrt(2.0))
    XCTAssert(isCloseRelative(cBW.a1, cQ.a1, 0.001), "BP a1")
    XCTAssert(isCloseRelative(cBW.a2, cQ.a2, 0.001), "BP a2")
    XCTAssert(isCloseRelative(cBW.b0, cQ.b0, 0.001), "BP b0")
    XCTAssertEqual(cBW.b1, 0.0, "BP b1 must be 0")
    XCTAssertEqual(cQ.b1, 0.0, "BP b1 must be 0")
    XCTAssert(isCloseRelative(cBW.b2, cQ.b2, 0.001), "BP b2")
  }

  // Rust: notch_bw_vs_q
  func testNotchBWvsQ() throws {
    let cBW = try makeCoeffs(type: .notch, freq: 100.0, bandwidth: 1.0)
    let cQ = try makeCoeffs(type: .notch, freq: 100.0, q: sqrt(2.0))
    XCTAssert(isCloseRelative(cBW.a1, cQ.a1, 0.001), "Notch a1")
    XCTAssert(isCloseRelative(cBW.a2, cQ.a2, 0.001), "Notch a2")
    XCTAssert(isCloseRelative(cBW.b0, cQ.b0, 0.001), "Notch b0")
    XCTAssert(isCloseRelative(cBW.b1, cQ.b1, 0.001), "Notch b1")
    XCTAssert(isCloseRelative(cBW.b2, cQ.b2, 0.001), "Notch b2")
  }

  // Rust: allpass_bw_vs_q
  func testAllpassBWvsQ() throws {
    let cBW = try makeCoeffs(type: .allpass, freq: 100.0, bandwidth: 1.0)
    let cQ = try makeCoeffs(type: .allpass, freq: 100.0, q: sqrt(2.0))
    XCTAssert(isCloseRelative(cBW.a1, cQ.a1, 0.001), "AP a1")
    XCTAssert(isCloseRelative(cBW.a2, cQ.a2, 0.001), "AP a2")
    XCTAssert(isCloseRelative(cBW.b0, cQ.b0, 0.001), "AP b0")
    XCTAssert(isCloseRelative(cBW.b1, cQ.b1, 0.001), "AP b1")
    XCTAssert(isCloseRelative(cBW.b2, cQ.b2, 0.001), "AP b2")
  }

  // Rust: make_highshelf_fo — gain=-12, freq=100
  func testHighshelfFO() throws {
    let coeffs = try makeCoeffs(type: .highshelfFO, freq: 100.0, gain: -12.0)
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 10000.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 1.0, fs: fs)
    XCTAssert(isClose(gf0, -6.0, 0.1), "HSFO f0=\(gf0)")
    XCTAssert(isClose(glf, 0.0, 0.1), "HSFO lf=\(glf)")
    XCTAssert(isClose(ghf, -12.0, 0.1), "HSFO hf=\(ghf)")
  }

  // Rust: make_lowshelf_fo — gain=-12, freq=100
  func testLowshelfFO() throws {
    let coeffs = try makeCoeffs(type: .lowshelfFO, freq: 100.0, gain: -12.0)
    let (gf0, _) = gainAndPhase(coeffs: coeffs, f: 100.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 10000.0, fs: fs)
    let (glf, _) = gainAndPhase(coeffs: coeffs, f: 1.0, fs: fs)
    XCTAssert(isClose(gf0, -6.0, 0.1), "LSFO f0=\(gf0)")
    XCTAssert(isClose(glf, -12.0, 0.1), "LSFO lf=\(glf)")
    XCTAssert(isClose(ghf, 0.0, 0.1), "LSFO hf=\(ghf)")
  }

  // Rust: make_lt — LinkwitzTransform act=100/1.2 → target=25/0.7
  func testLinkwitzTransform() throws {
    let coeffs = try makeCoeffs(
      type: .linkwitzTransform,
      freqAct: 100.0, qAct: 1.2, freqTarget: 25.0, qTarget: 0.7)
    let (g10, _) = gainAndPhase(coeffs: coeffs, f: 10.0, fs: fs)
    let (g87, _) = gainAndPhase(coeffs: coeffs, f: 87.0, fs: fs)
    let (g123, _) = gainAndPhase(coeffs: coeffs, f: 123.0, fs: fs)
    let (ghf, _) = gainAndPhase(coeffs: coeffs, f: 10000.0, fs: fs)
    XCTAssert(isClose(g10, 23.9, 0.1), "LT 10Hz=\(g10)")
    XCTAssert(isClose(g87, 0.0, 0.1), "LT 87Hz=\(g87)")
    XCTAssert(isClose(g123, -2.4, 0.1), "LT 123Hz=\(g123)")
    XCTAssert(isClose(ghf, 0.0, 0.1), "LT hf=\(ghf)")
  }

  // Rust: check_freq_q — validation tests
  func testValidateFreqQ() throws {
    let fs48 = 48000
    // OK: freq=1000, Q=2
    XCTAssertNoThrow(
      try makeCoeffs(type: .peaking, freq: 1000.0, q: 2.0, gain: 1.23, sampleRate: fs48))
    // Bad: Q=0
    XCTAssertThrowsError(
      try makeCoeffs(type: .peaking, freq: 1000.0, q: 0.0, gain: 1.23, sampleRate: fs48))
    // Bad: freq > Nyquist
    XCTAssertThrowsError(
      try makeCoeffs(type: .peaking, freq: 25000.0, q: 1.0, gain: 1.23, sampleRate: fs48))
    // Bad: freq = 0
    XCTAssertThrowsError(
      try makeCoeffs(type: .peaking, freq: 0.0, q: 1.0, gain: 1.23, sampleRate: fs48))
  }

  // Rust: check_slope — validation tests
  func testValidateSlope() throws {
    let fs48 = 48000
    // OK: slope=5
    XCTAssertNoThrow(
      try makeCoeffs(type: .highshelf, freq: 1000.0, gain: 1.23, slope: 5.0, sampleRate: fs48))
    // Bad: slope=0
    XCTAssertThrowsError(
      try makeCoeffs(type: .highshelf, freq: 1000.0, gain: 1.23, slope: 0.0, sampleRate: fs48))
    // Bad: slope=15 (> 12)
    XCTAssertThrowsError(
      try makeCoeffs(type: .highshelf, freq: 1000.0, gain: 1.23, slope: 15.0, sampleRate: fs48))
  }
}
