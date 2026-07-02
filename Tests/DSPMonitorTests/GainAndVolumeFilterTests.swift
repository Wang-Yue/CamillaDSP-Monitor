import Foundation
import Testing

@testable import DSPAudio
@testable import DSPConfig
@testable import DSPFilters

@Suite struct GainAndVolumeFilterTests {

  // MARK: - Helpers

  private func makeGainParameters(configure: (inout GainParameters) -> Void) -> GainParameters {
    var params = GainParameters()
    configure(&params)
    return params
  }

  // MARK: - Gain Tests

  /// 0 dB + invert: [-0.5, 0.0, 0.5] → [0.5, 0.0, -0.5]
  @Test func GainInvert() throws {
    let params = makeGainParameters {
      $0.gain = 0.0
      $0.scale = .dB
      $0.inverted = true
    }
    let filter = GainFilter(parameters: params)

    var waveform: [PrcFmt] = [-0.5, 0.0, 0.5]
    filter.process(waveform: &waveform)

    #expect(abs(waveform[0] - 0.5) <= 1e-10)
    #expect(abs(waveform[1] - 0.0) <= 1e-10)
    #expect(abs(waveform[2] - -0.5) <= 1e-10)
  }

  /// +20 dB (10× amplitude): [-0.5, 0.0, 0.5] → [-5.0, 0.0, 5.0]
  @Test func GainAmplify() throws {
    let params = makeGainParameters {
      $0.gain = 20.0
      $0.scale = .dB
    }
    let filter = GainFilter(parameters: params)

    var waveform: [PrcFmt] = [-0.5, 0.0, 0.5]
    filter.process(waveform: &waveform)

    #expect(abs(waveform[0] - -5.0) <= 1e-6)
    #expect(abs(waveform[1] - 0.0) <= 1e-10)
    #expect(abs(waveform[2] - 5.0) <= 1e-6)
  }

  /// Muted filter: every sample becomes 0.
  @Test func GainMute() throws {
    let params = makeGainParameters {
      $0.gain = 0.0
      $0.mute = true
    }
    let filter = GainFilter(parameters: params)

    var waveform: [PrcFmt] = [-0.5, 0.0, 0.5, 1.0, -1.0]
    filter.process(waveform: &waveform)

    for sample in waveform {
      #expect(sample == 0.0)
    }
  }

  /// Linear scale 0.5: [1.0] → [0.5]
  @Test func GainLinearScale() throws {
    let params = makeGainParameters {
      $0.gain = 0.5
      $0.scale = .linear
    }
    let filter = GainFilter(parameters: params)

    var waveform: [PrcFmt] = [1.0]
    filter.process(waveform: &waveform)
    #expect(abs(waveform[0] - 0.5) <= 1e-10)
  }

  // MARK: - Volume Tests

  private func makeVolumeFilter(
    rampTimeMs: Double = 0.0,
    limit: Double = 50.0,
    currentVolume: Double = 0.0,
    mute: Bool = false,
    chunkSize: Int = 4,
    sampleRate: Int = 44100,
    fader: Fader = .main
  ) -> (VolumeFilter, ProcessingParameters) {
    let params = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    params.setTargetVolume(currentVolume, for: fader)
    params.setMuted(mute, for: fader)

    let volParams = VolumeParameters(rampTime: rampTimeMs, limit: limit, fader: fader)
    let filter = VolumeFilter(
      parameters: volParams,
      sampleRate: sampleRate,
      chunkSize: chunkSize,
      processingParameters: params
    )
    return (filter, params)
  }

  private func process(_ filter: VolumeFilter, _ waveform: inout [PrcFmt]) {
    filter.prepareChunk()
    filter.process(waveform: &waveform)
    filter.advanceRamp()
  }

  /// 0 dB → signal unchanged.
  @Test func VolumeUnityGain() throws {
    let (filter, _) = makeVolumeFilter(currentVolume: 0.0)
    var waveform: [PrcFmt] = [1.0, -0.5, 0.25, 0.0]
    let original = waveform
    process(filter, &waveform)
    for i in 0..<waveform.count {
      #expect(abs(waveform[i] - original[i]) <= 1e-10)
    }
  }

  /// −20 dB → signal × 0.1.
  @Test func VolumeAttenuation() throws {
    let (filter, _) = makeVolumeFilter(currentVolume: -20.0)
    var waveform: [PrcFmt] = [1.0, -1.0, 0.5, -0.5]
    process(filter, &waveform)
    let gain = PrcFmt.fromDB(-20.0)
    #expect(abs(waveform[0] - 1.0 * gain) <= 1e-10)
    #expect(abs(waveform[1] - -1.0 * gain) <= 1e-10)
    #expect(abs(waveform[2] - 0.5 * gain) <= 1e-10)
    #expect(abs(waveform[3] - -0.5 * gain) <= 1e-10)
  }

  /// Mute → output is silence.
  @Test func VolumeMuteRampsToZero() throws {
    let (filter, _) = makeVolumeFilter(currentVolume: 0.0, mute: true)
    var waveform: [PrcFmt] = [1.0, 0.5, -0.5, -1.0]
    process(filter, &waveform)
    for sample in waveform {
      #expect(abs(sample - 0.0) <= 1e-10)
    }
  }

  /// Ramped volume change over chunks
  @Test func VolumeRamp() throws {
    let chunkSize = 4
    let sampleRate = 44100
    // ramptimeInChunks = 2
    let rampTimeMs = 1000.0 * Double(chunkSize) / Double(sampleRate) * 2.0

    let (filter, params) = makeVolumeFilter(
      rampTimeMs: rampTimeMs,
      currentVolume: 0.0,
      chunkSize: chunkSize,
      sampleRate: sampleRate
    )

    // Process chunk 0 (baseline unity)
    var chunk0: [PrcFmt] = [1.0, 1.0, 1.0, 1.0]
    process(filter, &chunk0)
    for i in 0..<chunkSize {
      #expect(abs(chunk0[i] - 1.0) <= 1e-10)
    }

    // Set target to -20 dB
    params.setTargetVolume(-20.0, for: .main)

    // Process ramp chunk 1
    var chunk1: [PrcFmt] = [1.0, 1.0, 1.0, 1.0]
    process(filter, &chunk1)

    let gain0dB = PrcFmt.fromDB(0.0)
    let gainM20dB = PrcFmt.fromDB(-20.0)
    for i in 0..<chunkSize {
      #expect(chunk1[i] <= gain0dB + 1e-6)
      #expect(chunk1[i] >= gainM20dB - 1e-6)
    }
    #expect(chunk1[0] > chunk1[chunkSize - 1])

    // Process ramp chunk 2
    var chunk2: [PrcFmt] = [1.0, 1.0, 1.0, 1.0]
    process(filter, &chunk2)

    #expect(chunk2[chunkSize - 1] < chunk1[chunkSize - 1])
    #expect(chunk2[chunkSize - 1] >= gainM20dB - 1e-6)

    // Process chunk 3 (ramp complete)
    var chunk3: [PrcFmt] = [1.0, 1.0, 1.0, 1.0]
    process(filter, &chunk3)

    for i in 0..<chunkSize {
      #expect(abs(chunk3[i] - gainM20dB) <= 1e-6)
    }
  }

  /// Change detection threshold
  @Test func VolumeChangeThreshold() throws {
    let (filter, params) = makeVolumeFilter(rampTimeMs: 0.0, currentVolume: 0.0)

    var wave1: [PrcFmt] = [1.0, 1.0, 1.0, 1.0]
    process(filter, &wave1)

    // below 0.01 threshold
    params.setTargetVolume(0.005, for: .main)
    var wave2: [PrcFmt] = [1.0, 1.0, 1.0, 1.0]
    process(filter, &wave2)
    for sample in wave2 {
      #expect(abs(sample - 1.0) <= 1e-10)
    }

    // above threshold
    params.setTargetVolume(0.02, for: .main)
    var wave3: [PrcFmt] = [1.0, 1.0, 1.0, 1.0]
    process(filter, &wave3)
    let expectedGain = PrcFmt.fromDB(0.02)
    for sample in wave3 {
      #expect(abs(sample - expectedGain) <= 1e-6)
    }
  }

  /// Safety limit clamping
  @Test func VolumeLimit() throws {
    let (filter, params) = makeVolumeFilter(rampTimeMs: 0.0, limit: 10.0, currentVolume: 0.0)

    params.setTargetVolume(20.0, for: .main)
    var waveform: [PrcFmt] = [1.0, 1.0, 1.0, 1.0]
    process(filter, &waveform)

    let expectedGain = PrcFmt.fromDB(10.0)
    for sample in waveform {
      #expect(abs(sample - expectedGain) <= 1e-6)
    }
  }

  /// updateParameters clamps volume
  @Test func VolumeUpdateParametersClampsToLimit() throws {
    let (filter, params) = makeVolumeFilter(rampTimeMs: 0.0, limit: 50.0, currentVolume: 20.0)

    var wave1: [PrcFmt] = [1.0, 1.0, 1.0, 1.0]
    process(filter, &wave1)

    let newParams = VolumeParameters(rampTime: 0.0, limit: 10.0)
    filter.updateParameters(.volume(newParams), sampleRate: 44100)

    params.setTargetVolume(10.0, for: .main)
    var wave2: [PrcFmt] = [1.0, 1.0, 1.0, 1.0]
    process(filter, &wave2)

    let expectedGain = PrcFmt.fromDB(10.0)
    for sample in wave2 {
      #expect(abs(sample - expectedGain) <= 1e-6)
    }
  }
}
