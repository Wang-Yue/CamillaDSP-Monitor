// Tests for the two scalar filters CamillaDSP-Monitor actually uses (Gain
// and the implicit master Volume filter). Lifted from the upstream Swift
// port's BasicFilterTests; the Delay/Limiter/Dither/DiffEq/Compressor
// sections are dropped because those filters were pruned along with the
// rest of the unused DSP surface.

import XCTest

@testable import CamillaDSPLib

final class GainAndVolumeFilterTests: XCTestCase {

  // MARK: - Helpers

  private func makeFilterConfig(type: FilterType, configure: (inout FilterParameters) -> Void)
    -> FilterConfig
  {
    var params = FilterParameters()
    configure(&params)
    return FilterConfig(type: type, parameters: params)
  }

  // MARK: - Gain Tests

  /// 0 dB + invert: [-0.5, 0.0, 0.5] → [0.5, 0.0, -0.5]
  func testGainInvert() throws {
    let config = makeFilterConfig(type: .gain) {
      $0.gain = 0.0
      $0.scale = .dB
      $0.inverted = true
    }
    let filter = GainFilter(name: "gain_invert", config: config)

    var waveform: [PrcFmt] = [-0.5, 0.0, 0.5]
    try filter.process(waveform: &waveform)

    XCTAssertEqual(waveform[0], 0.5, accuracy: 1e-10)
    XCTAssertEqual(waveform[1], 0.0, accuracy: 1e-10)
    XCTAssertEqual(waveform[2], -0.5, accuracy: 1e-10)
  }

  /// +20 dB (10× amplitude): [-0.5, 0.0, 0.5] → [-5.0, 0.0, 5.0]
  func testGainAmplify() throws {
    let config = makeFilterConfig(type: .gain) {
      $0.gain = 20.0
      $0.scale = .dB
    }
    let filter = GainFilter(name: "gain_amplify", config: config)

    var waveform: [PrcFmt] = [-0.5, 0.0, 0.5]
    try filter.process(waveform: &waveform)

    XCTAssertEqual(waveform[0], -5.0, accuracy: 1e-6)
    XCTAssertEqual(waveform[1], 0.0, accuracy: 1e-10)
    XCTAssertEqual(waveform[2], 5.0, accuracy: 1e-6)
  }

  /// Muted filter: every sample becomes 0.
  func testGainMute() throws {
    let config = makeFilterConfig(type: .gain) {
      $0.gain = 0.0
      $0.mute = true
    }
    let filter = GainFilter(name: "gain_mute", config: config)

    var waveform: [PrcFmt] = [-0.5, 0.0, 0.5, 1.0, -1.0]
    try filter.process(waveform: &waveform)

    for sample in waveform {
      XCTAssertEqual(sample, 0.0)
    }
  }

  /// Linear scale 0.5: [1.0] → [0.5]
  func testGainLinearScale() throws {
    let config = makeFilterConfig(type: .gain) {
      $0.gain = 0.5
      $0.scale = .linear
    }
    let filter = GainFilter(name: "gain_linear", config: config)

    var waveform: [PrcFmt] = [1.0]
    try filter.process(waveform: &waveform)
    XCTAssertEqual(waveform[0], 0.5, accuracy: 1e-10)
  }

  // MARK: - Volume Tests

  /// Build a VolumeFilter the same way `Pipeline` does for the implicit
  /// master volume slot — direct init, fed by a fresh ProcessingParameters.
  private func makeVolumeFilter(
    rampTimeMs: Double = 400.0,
    limit: Double = 50.0,
    currentVolume: Double = 0.0,
    mute: Bool = false,
    chunkSize: Int = 4,
    sampleRate: Int = 44100
  ) -> (VolumeFilter, ProcessingParameters) {
    let params = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    params.targetVolume = currentVolume
    params.isMuted = mute
    let filter = VolumeFilter(
      name: "test_volume",
      rampTimeMs: rampTimeMs,
      limit: limit,
      currentVolume: currentVolume,
      mute: mute,
      chunkSize: chunkSize,
      sampleRate: sampleRate,
      processingParameters: params
    )
    return (filter, params)
  }

  /// 0 dB → signal unchanged.
  func testVolumeUnityGain() throws {
    let (filter, _) = makeVolumeFilter(rampTimeMs: 0.0, currentVolume: 0.0)
    var waveform: [PrcFmt] = [1.0, -0.5, 0.25, 0.0]
    let original = waveform
    try filter.process(waveform: &waveform)
    for i in 0..<waveform.count {
      XCTAssertEqual(waveform[i], original[i], accuracy: 1e-10)
    }
  }

  /// −20 dB → signal × 0.1.
  func testVolumeAttenuation() throws {
    let (filter, _) = makeVolumeFilter(rampTimeMs: 0.0, currentVolume: -20.0)
    var waveform: [PrcFmt] = [1.0, -1.0, 0.5, -0.5]
    try filter.process(waveform: &waveform)
    let gain = PrcFmt.fromDB(-20.0)
    XCTAssertEqual(waveform[0], 1.0 * gain, accuracy: 1e-10)
    XCTAssertEqual(waveform[1], -1.0 * gain, accuracy: 1e-10)
    XCTAssertEqual(waveform[2], 0.5 * gain, accuracy: 1e-10)
    XCTAssertEqual(waveform[3], -0.5 * gain, accuracy: 1e-10)
  }

  /// Mute → output is silence (with rampTime=0, the gain snaps to 0).
  func testVolumeMuteRampsToZero() throws {
    let (filter, _) = makeVolumeFilter(rampTimeMs: 0.0, currentVolume: 0.0, mute: true)
    var waveform: [PrcFmt] = [1.0, 0.5, -0.5, -1.0]
    try filter.process(waveform: &waveform)
    for sample in waveform {
      XCTAssertEqual(sample, 0.0, accuracy: 1e-10)
    }
  }

  /// Volume change below the 0.01 dB threshold should not retrigger.
  func testVolumeChangeThreshold() throws {
    let (filter, params) = makeVolumeFilter(rampTimeMs: 0.0, currentVolume: 0.0)

    var wave1: [PrcFmt] = [1.0, 1.0, 1.0, 1.0]
    try filter.process(waveform: &wave1)

    params.targetVolume = 0.005
    var wave2: [PrcFmt] = [1.0, 1.0, 1.0, 1.0]
    try filter.process(waveform: &wave2)
    for i in 0..<wave2.count {
      XCTAssertEqual(wave2[i], 1.0, accuracy: 1e-10)
    }

    params.targetVolume = 0.02
    var wave3: [PrcFmt] = [1.0, 1.0, 1.0, 1.0]
    try filter.process(waveform: &wave3)
    let expected = PrcFmt.fromDB(0.02)
    for i in 0..<wave3.count {
      XCTAssertEqual(wave3[i], expected, accuracy: 1e-6)
    }
  }

  /// Target volume above the configured limit is clamped.
  func testVolumeLimit() throws {
    let (filter, params) = makeVolumeFilter(rampTimeMs: 0.0, limit: 10.0, currentVolume: 0.0)
    params.targetVolume = 20.0
    var waveform: [PrcFmt] = [1.0, 1.0, 1.0, 1.0]
    try filter.process(waveform: &waveform)

    let expected = PrcFmt.fromDB(10.0)
    for i in 0..<waveform.count {
      XCTAssertEqual(waveform[i], expected, accuracy: 1e-6)
    }
  }

  /// Lowering the limit via updateParameters clamps the current volume.
  func testVolumeUpdateParametersClampsToLimit() throws {
    let (filter, params) = makeVolumeFilter(rampTimeMs: 0.0, limit: 50.0, currentVolume: 20.0)

    var wave1: [PrcFmt] = [1.0, 1.0, 1.0, 1.0]
    try filter.process(waveform: &wave1)

    var newParams = FilterParameters()
    newParams.rampTime = 0.0
    newParams.limit = 10.0
    let newConfig = FilterConfig(type: .volume, parameters: newParams)
    filter.updateParameters(newConfig)

    params.targetVolume = 10.0
    var wave2: [PrcFmt] = [1.0, 1.0, 1.0, 1.0]
    try filter.process(waveform: &wave2)
    let expected = PrcFmt.fromDB(10.0)
    for i in 0..<wave2.count {
      XCTAssertEqual(wave2[i], expected, accuracy: 1e-6)
    }
  }
}
