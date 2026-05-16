// Tests for the two scalar filters DSPMonitor actually uses (Gain
// and the implicit master Volume filter). Lifted from the upstream Swift
// port's BasicFilterTests; the Delay/Limiter/Dither/DiffEq/Compressor
// sections are dropped because those filters were pruned along with the
// rest of the unused DSP surface.

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

  /// Build a VolumeFilter the same way `Pipeline` does for the implicit
  /// master volume slot — direct init, fed by a fresh ProcessingParameters.
  private func makeVolumeFilter(
    currentVolume: Double = 0.0,
    mute: Bool = false
  ) -> (VolumeFilter, ProcessingParameters) {
    let params = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    params.targetVolume = currentVolume
    params.isMuted = mute
    let filter = VolumeFilter(
      processingParameters: params
    )
    return (filter, params)
  }

  /// 0 dB → signal unchanged.
  @Test func VolumeUnityGain() throws {
    let (filter, _) = makeVolumeFilter(currentVolume: 0.0)
    var waveform: [PrcFmt] = [1.0, -0.5, 0.25, 0.0]
    let original = waveform
    filter.process(waveform: &waveform)
    for i in 0..<waveform.count {
      #expect(abs(waveform[i] - original[i]) <= 1e-10)
    }
  }

  /// −20 dB → signal × 0.1.
  @Test func VolumeAttenuation() throws {
    let (filter, _) = makeVolumeFilter(currentVolume: -20.0)
    var waveform: [PrcFmt] = [1.0, -1.0, 0.5, -0.5]
    filter.process(waveform: &waveform)
    let gain = PrcFmt.fromDB(-20.0)
    #expect(abs(waveform[0] - 1.0 * gain) <= 1e-10)
    #expect(abs(waveform[1] - -1.0 * gain) <= 1e-10)
    #expect(abs(waveform[2] - 0.5 * gain) <= 1e-10)
    #expect(abs(waveform[3] - -0.5 * gain) <= 1e-10)
  }

  /// Mute → output is silence (with rampTime=0, the gain snaps to 0).
  @Test func VolumeMuteRampsToZero() throws {
    let (filter, _) = makeVolumeFilter(currentVolume: 0.0, mute: true)
    var waveform: [PrcFmt] = [1.0, 0.5, -0.5, -1.0]
    filter.process(waveform: &waveform)
    for sample in waveform {
      #expect(abs(sample - 0.0) <= 1e-10)
    }
  }

}
