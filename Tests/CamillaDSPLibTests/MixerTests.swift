// CamillaDSP-Swift: Comprehensive Mixer Tests
// Mirrors the CamillaDSP Rust test suite for AudioMixer / MixerConfig behavior.

import XCTest

@testable import CamillaDSPLib

final class MixerTests: XCTestCase {

  // MARK: - Helpers

  /// Tolerance for floating-point comparisons
  private let accuracy: PrcFmt = 1e-9

  /// Build a simple constant waveform with `channels` channels, each filled with `value`.
  private func makeConstantChunk(frames: Int = 8, channels: Int, value: PrcFmt = 1.0) -> AudioChunk
  {
    let waveforms = Array(repeating: Array(repeating: value, count: frames), count: channels)
    return AudioChunk(waveforms: waveforms)
  }

  /// Assert every sample in `waveform` equals `expected` within `accuracy`.
  private func assertAllSamples(
    _ waveform: [PrcFmt],
    equal expected: PrcFmt,
    accuracy: PrcFmt = 1e-9,
    file: StaticString = #filePath,
    line: UInt = #line
  ) {
    for (i, sample) in waveform.enumerated() {
      XCTAssertEqual(
        sample, expected, accuracy: accuracy,
        "waveform[\(i)] expected \(expected) but got \(sample)",
        file: file, line: line
      )
    }
  }

  /// Assert every sample in `waveform` is zero.
  private func assertSilence(
    _ waveform: [PrcFmt],
    file: StaticString = #filePath,
    line: UInt = #line
  ) {
    assertAllSamples(waveform, equal: 0.0, accuracy: accuracy, file: file, line: line)
  }

  // MARK: - 1. testMixerConstruction2to4

  /// Build a 2-in / 4-out mixer: each output channel sources from one input at 0 dB.
  /// Verify channelsIn, channelsOut, and that all four output channels pass through at
  /// linear gain 1.0 (the linear equivalent of 0 dB).
  func testMixerConstruction2to4() {
    let config = MixerConfig(
      channelsIn: 2,
      channelsOut: 4,
      mapping: [
        MixerMapping(dest: 0, sources: [MixerSource(channel: 0, gain: 0.0)]),
        MixerMapping(dest: 1, sources: [MixerSource(channel: 1, gain: 0.0)]),
        MixerMapping(dest: 2, sources: [MixerSource(channel: 0, gain: 0.0)]),
        MixerMapping(dest: 3, sources: [MixerSource(channel: 1, gain: 0.0)]),
      ]
    )
    let mixer = AudioMixer(name: "2to4", config: config, chunkSize: 2048)

    XCTAssertEqual(mixer.channelsIn, 2)
    XCTAssertEqual(mixer.channelsOut, 4)

    // Feed two channels of 1.0 — 0 dB gain must produce exactly 1.0 out.
    let input = makeConstantChunk(channels: 2, value: 1.0)
    let output = mixer.process(chunk: input)

    XCTAssertEqual(output.channels, 4)
    XCTAssertEqual(output.frames, input.frames)

    let expectedLinear = PrcFmt.fromDB(0.0)  // == 1.0
    for ch in 0..<4 {
      assertAllSamples(output.waveforms[ch], equal: expectedLinear, accuracy: accuracy)
    }
  }

  // MARK: - 2. testMixerMutedMapping

  /// Mappings for outputs 0 and 2 have `mute = true`.
  /// Those outputs must produce silence; outputs 1 and 3 (unmuted) must pass through.
  func testMixerMutedMapping() {
    let config = MixerConfig(
      channelsIn: 2,
      channelsOut: 4,
      mapping: [
        MixerMapping(dest: 0, sources: [MixerSource(channel: 0, gain: 0.0)], mute: true),
        MixerMapping(dest: 1, sources: [MixerSource(channel: 1, gain: 0.0)]),
        MixerMapping(dest: 2, sources: [MixerSource(channel: 0, gain: 0.0)], mute: true),
        MixerMapping(dest: 3, sources: [MixerSource(channel: 1, gain: 0.0)]),
      ]
    )
    let mixer = AudioMixer(name: "muted_mapping", config: config, chunkSize: 2048)
    let input = makeConstantChunk(channels: 2, value: 1.0)
    let output = mixer.process(chunk: input)

    XCTAssertEqual(output.channels, 4)

    // Muted outputs → silence
    assertSilence(output.waveforms[0])
    assertSilence(output.waveforms[2])

    // Unmuted outputs → 0 dB pass-through
    assertAllSamples(output.waveforms[1], equal: 1.0, accuracy: accuracy)
    assertAllSamples(output.waveforms[3], equal: 1.0, accuracy: accuracy)
  }

  // MARK: - 3. testMixerMutedSource

  /// A source entry with `mute = true` must not contribute to the output.
  /// Output channel 0 receives one muted source and one live source; only the live
  /// source should appear in the result.
  func testMixerMutedSource() {
    let config = MixerConfig(
      channelsIn: 2,
      channelsOut: 1,
      mapping: [
        MixerMapping(
          dest: 0,
          sources: [
            MixerSource(channel: 0, gain: 0.0, mute: true),  // muted – should not contribute
            MixerSource(channel: 1, gain: 0.0),  // live – should contribute
          ])
      ]
    )
    let mixer = AudioMixer(name: "muted_source", config: config, chunkSize: 2048)

    let input = AudioChunk(waveforms: [
      [1.0, 1.0, 1.0, 1.0],  // channel 0 (muted source): value 1.0
      [0.5, 0.5, 0.5, 0.5],  // channel 1 (live source):  value 0.5
    ])
    let output = mixer.process(chunk: input)

    XCTAssertEqual(output.channels, 1)
    // Only the live source at 0 dB (gain 1.0) contributes → 0.5
    assertAllSamples(output.waveforms[0], equal: 0.5, accuracy: accuracy)
  }

  // MARK: - 4. testMixerStereoToMono

  /// Sum L + R at -6 dB each.  Two channels of 1.0 → one channel at ~1.0 (2 × 0.5012).
  func testMixerStereoToMono() {
    let config = MixerConfig(
      channelsIn: 2,
      channelsOut: 1,
      mapping: [
        MixerMapping(
          dest: 0,
          sources: [
            MixerSource(channel: 0, gain: -6.0),
            MixerSource(channel: 1, gain: -6.0),
          ])
      ]
    )
    let mixer = AudioMixer(name: "stereo_to_mono", config: config, chunkSize: 2048)

    let input = AudioChunk(waveforms: [
      [1.0, 1.0, 1.0, 1.0],
      [1.0, 1.0, 1.0, 1.0],
    ])
    let output = mixer.process(chunk: input)

    XCTAssertEqual(output.channels, 1)
    XCTAssertEqual(output.frames, 4)

    let expected = PrcFmt.fromDB(-6.0) * 2.0
    assertAllSamples(output.waveforms[0], equal: expected, accuracy: 1e-6)
  }

  // MARK: - 5. testMixerMonoToStereo

  /// Duplicate a mono input to both stereo outputs at 0 dB.
  /// Both output channels must be identical and equal to the input.
  func testMixerMonoToStereo() {
    let config = MixerConfig(
      channelsIn: 1,
      channelsOut: 2,
      mapping: [
        MixerMapping(dest: 0, sources: [MixerSource(channel: 0, gain: 0.0)]),
        MixerMapping(dest: 1, sources: [MixerSource(channel: 0, gain: 0.0)]),
      ]
    )
    let mixer = AudioMixer(name: "mono_to_stereo", config: config, chunkSize: 2048)

    let inputSamples: [PrcFmt] = [0.25, -0.5, 0.75, -1.0]
    let input = AudioChunk(waveforms: [inputSamples])
    let output = mixer.process(chunk: input)

    XCTAssertEqual(output.channels, 2)
    XCTAssertEqual(output.frames, 4)

    for i in 0..<inputSamples.count {
      XCTAssertEqual(output.waveforms[0][i], inputSamples[i], accuracy: accuracy)
      XCTAssertEqual(output.waveforms[1][i], inputSamples[i], accuracy: accuracy)
    }
    // Both channels must be identical
    XCTAssertEqual(output.waveforms[0], output.waveforms[1])
  }

  // MARK: - 6. testMixer4to2Downmix

  /// 4-channel to stereo downmix with different gains per source.
  ///   out[0] = in[0]*1.0 + in[2]*0.5   (0 dB + -6 dB)
  ///   out[1] = in[1]*1.0 + in[3]*0.5
  func testMixer4to2Downmix() {
    let config = MixerConfig(
      channelsIn: 4,
      channelsOut: 2,
      mapping: [
        MixerMapping(
          dest: 0,
          sources: [
            MixerSource(channel: 0, gain: 0.0),
            MixerSource(channel: 2, gain: -6.0),
          ]),
        MixerMapping(
          dest: 1,
          sources: [
            MixerSource(channel: 1, gain: 0.0),
            MixerSource(channel: 3, gain: -6.0),
          ]),
      ]
    )
    let mixer = AudioMixer(name: "4to2_downmix", config: config, chunkSize: 2048)

    // All four channels carry a constant 1.0
    let input = makeConstantChunk(channels: 4, value: 1.0)
    let output = mixer.process(chunk: input)

    XCTAssertEqual(output.channels, 2)

    // 1.0 (0 dB) + PrcFmt.fromDB(-6.0) ≈ 1.0 + 0.5012
    let expected = 1.0 + PrcFmt.fromDB(-6.0)
    assertAllSamples(output.waveforms[0], equal: expected, accuracy: 1e-6)
    assertAllSamples(output.waveforms[1], equal: expected, accuracy: 1e-6)
  }

  // MARK: - 7. testMixerWithInvertedSource

  /// A source with `inverted = true` must be subtracted from the output.
  /// Two equal sources where one is inverted should cancel to zero.
  func testMixerWithInvertedSource() {
    let config = MixerConfig(
      channelsIn: 1,
      channelsOut: 1,
      mapping: [
        MixerMapping(
          dest: 0,
          sources: [
            MixerSource(channel: 0, gain: 0.0),  // +1.0
            MixerSource(channel: 0, gain: 0.0, inverted: true),  // -1.0
          ])
      ]
    )
    let mixer = AudioMixer(name: "inverted_source", config: config, chunkSize: 2048)

    let input = AudioChunk(waveforms: [[1.0, -0.5, 0.25, 0.8]])
    let output = mixer.process(chunk: input)

    XCTAssertEqual(output.channels, 1)
    assertSilence(output.waveforms[0])
  }

  // MARK: - 8. testMixerIdentity

  /// A 2-to-2 identity mixer: channel 0 → output 0, channel 1 → output 1, both at 0 dB.
  /// The output waveforms must be exactly equal to the inputs.
  func testMixerIdentity() {
    let config = MixerConfig(
      channelsIn: 2,
      channelsOut: 2,
      mapping: [
        MixerMapping(dest: 0, sources: [MixerSource(channel: 0, gain: 0.0)]),
        MixerMapping(dest: 1, sources: [MixerSource(channel: 1, gain: 0.0)]),
      ]
    )
    let mixer = AudioMixer(name: "identity", config: config, chunkSize: 2048)

    let ch0: [PrcFmt] = [0.1, -0.2, 0.3, -0.4]
    let ch1: [PrcFmt] = [0.5, -0.6, 0.7, -0.8]
    let input = AudioChunk(waveforms: [ch0, ch1])
    let output = mixer.process(chunk: input)

    XCTAssertEqual(output.channels, 2)
    for i in 0..<ch0.count {
      XCTAssertEqual(output.waveforms[0][i], ch0[i], accuracy: accuracy)
      XCTAssertEqual(output.waveforms[1][i], ch1[i], accuracy: accuracy)
    }
  }

  // MARK: - 9. testMixerChannelRouting

  /// Swap channels: input channel 1 → output 0, input channel 0 → output 1.
  func testMixerChannelRouting() {
    let config = MixerConfig(
      channelsIn: 2,
      channelsOut: 2,
      mapping: [
        MixerMapping(dest: 0, sources: [MixerSource(channel: 1, gain: 0.0)]),  // route ch1 → out0
        MixerMapping(dest: 1, sources: [MixerSource(channel: 0, gain: 0.0)]),  // route ch0 → out1
      ]
    )
    let mixer = AudioMixer(name: "channel_swap", config: config, chunkSize: 2048)

    let ch0: [PrcFmt] = [1.0, 2.0, 3.0, 4.0]
    let ch1: [PrcFmt] = [-1.0, -2.0, -3.0, -4.0]
    let input = AudioChunk(waveforms: [ch0, ch1])
    let output = mixer.process(chunk: input)

    XCTAssertEqual(output.channels, 2)
    // out[0] should be the original ch1, out[1] should be the original ch0
    for i in 0..<ch0.count {
      XCTAssertEqual(output.waveforms[0][i], ch1[i], accuracy: accuracy)
      XCTAssertEqual(output.waveforms[1][i], ch0[i], accuracy: accuracy)
    }
  }

  // MARK: - 10. testMixerGainAccuracy

  /// Verify exact gain conversions:
  ///   +6 dB  → linear ≈ 2.0
  ///   -6 dB  → linear ≈ 0.5012
  ///   muted source → effectively 0 (silence)
  func testMixerGainAccuracy() {
    let config = MixerConfig(
      channelsIn: 1,
      channelsOut: 3,
      mapping: [
        MixerMapping(dest: 0, sources: [MixerSource(channel: 0, gain: 6.0)]),  // +6 dB
        MixerMapping(dest: 1, sources: [MixerSource(channel: 0, gain: -6.0)]),  // -6 dB
        MixerMapping(dest: 2, sources: [MixerSource(channel: 0, gain: 0.0, mute: true)]),  // muted (−∞ dB)
      ]
    )
    let mixer = AudioMixer(name: "gain_accuracy", config: config, chunkSize: 2048)

    let input = AudioChunk(waveforms: [[1.0, 1.0, 1.0, 1.0]])
    let output = mixer.process(chunk: input)

    XCTAssertEqual(output.channels, 3)

    // +6 dB ≈ 1.9953 (not exactly 2.0)
    let gainPlus6 = PrcFmt.fromDB(6.0)
    XCTAssertEqual(
      gainPlus6, 2.0, accuracy: 0.01,
      "+6 dB should be approximately ×2")
    assertAllSamples(output.waveforms[0], equal: gainPlus6, accuracy: 1e-9)

    // -6 dB: approximately half amplitude
    let gainMinus6 = PrcFmt.fromDB(-6.0)
    XCTAssertEqual(
      gainMinus6, 0.5, accuracy: 1e-2,
      "-6 dB should be approximately ×0.5")
    assertAllSamples(output.waveforms[1], equal: gainMinus6, accuracy: 1e-9)

    // Muted source: output is silence (−∞ dB)
    assertSilence(output.waveforms[2])
  }

  // MARK: - 11. testMixerWithLinearScale

  /// Sources with `scale = .linear` should use the gain value directly as a
  /// linear multiplier rather than converting from dB.
  func testMixerWithLinearScale() {
    let config = MixerConfig(
      channelsIn: 2,
      channelsOut: 3,
      mapping: [
        MixerMapping(
          dest: 0,
          sources: [
            MixerSource(channel: 0, gain: 0.5, scale: .linear)  // ×0.5
          ]),
        MixerMapping(
          dest: 1,
          sources: [
            MixerSource(channel: 0, gain: 2.0, scale: .linear)  // ×2.0
          ]),
        MixerMapping(
          dest: 2,
          sources: [
            // Mix of both channels: linear 0.5 each
            MixerSource(channel: 0, gain: 0.5, scale: .linear),
            MixerSource(channel: 1, gain: 0.5, scale: .linear),
          ]),
      ]
    )
    let mixer = AudioMixer(name: "linear_scale", config: config, chunkSize: 2048)

    let input = AudioChunk(waveforms: [
      [1.0, 1.0, 1.0, 1.0],  // channel 0
      [1.0, 1.0, 1.0, 1.0],  // channel 1
    ])
    let output = mixer.process(chunk: input)

    XCTAssertEqual(output.channels, 3)

    // out[0]: input × 0.5 → 0.5
    assertAllSamples(output.waveforms[0], equal: 0.5, accuracy: accuracy)

    // out[1]: input × 2.0 → 2.0
    assertAllSamples(output.waveforms[1], equal: 2.0, accuracy: accuracy)

    // out[2]: ch0*0.5 + ch1*0.5 = 0.5 + 0.5 = 1.0
    assertAllSamples(output.waveforms[2], equal: 1.0, accuracy: accuracy)
  }

  // MARK: - Rust parity: check_make_mixer / check_make_mixer_muted

  /// Mirrors Rust check_make_mixer: 2-in/4-out, all sources at 0 dB (gain=0.0 dB → linear 1.0).
  /// Verifies channelsIn, channelsOut, and that each output passes an impulse through
  /// at exactly gain 1.0 (i.e. output equals input) — the structural equivalent of
  /// asserting mapping[dest][0] == MixerSource { channel, gain: 1.0 }.
  func testCheckMakeMixer() {
    // 2-in / 4-out: out0←ch0, out1←ch1, out2←ch0, out3←ch1, all at 0 dB
    let config = MixerConfig(
      channelsIn: 2,
      channelsOut: 4,
      mapping: [
        MixerMapping(dest: 0, sources: [MixerSource(channel: 0, gain: 0.0)]),
        MixerMapping(dest: 1, sources: [MixerSource(channel: 1, gain: 0.0)]),
        MixerMapping(dest: 2, sources: [MixerSource(channel: 0, gain: 0.0)]),
        MixerMapping(dest: 3, sources: [MixerSource(channel: 1, gain: 0.0)]),
      ]
    )
    let mixer = AudioMixer(name: "check_make_mixer", config: config, chunkSize: 2048)

    // Structural assertions (channelsIn, channelsOut)
    XCTAssertEqual(mixer.channelsIn, 2)
    XCTAssertEqual(mixer.channelsOut, 4)

    // Verify gain == 1.0 for each output: process a unit impulse and confirm output == input.
    // ch0 = [1, 0, 0, 0], ch1 = [0, 1, 0, 0]
    let input = AudioChunk(waveforms: [
      [1.0, 0.0, 0.0, 0.0],  // channel 0
      [0.0, 1.0, 0.0, 0.0],  // channel 1
    ])
    let output = mixer.process(chunk: input)

    XCTAssertEqual(output.channels, 4)
    XCTAssertEqual(output.frames, 4)

    // out[0] sources from ch0 at gain 1.0
    XCTAssertEqual(output.waveforms[0][0], 1.0, accuracy: accuracy, "out0 gain should be 1.0")
    XCTAssertEqual(output.waveforms[0][1], 0.0, accuracy: accuracy)

    // out[1] sources from ch1 at gain 1.0
    XCTAssertEqual(output.waveforms[1][0], 0.0, accuracy: accuracy)
    XCTAssertEqual(output.waveforms[1][1], 1.0, accuracy: accuracy, "out1 gain should be 1.0")

    // out[2] sources from ch0 at gain 1.0
    XCTAssertEqual(output.waveforms[2][0], 1.0, accuracy: accuracy, "out2 gain should be 1.0")
    XCTAssertEqual(output.waveforms[2][1], 0.0, accuracy: accuracy)

    // out[3] sources from ch1 at gain 1.0
    XCTAssertEqual(output.waveforms[3][0], 0.0, accuracy: accuracy)
    XCTAssertEqual(output.waveforms[3][1], 1.0, accuracy: accuracy, "out3 gain should be 1.0")
  }

  /// Mirrors Rust check_make_mixer_muted: mappings for dest 0 and 2 are muted.
  /// Muted outputs must produce empty source lists (silence); outputs 1 and 3 pass through at gain 1.0.
  func testCheckMakeMixerMuted() {
    let config = MixerConfig(
      channelsIn: 2,
      channelsOut: 4,
      mapping: [
        MixerMapping(dest: 0, sources: [MixerSource(channel: 0, gain: 0.0)], mute: true),
        MixerMapping(dest: 1, sources: [MixerSource(channel: 1, gain: 0.0)]),
        MixerMapping(dest: 2, sources: [MixerSource(channel: 0, gain: 0.0)], mute: true),
        MixerMapping(dest: 3, sources: [MixerSource(channel: 1, gain: 0.0)]),
      ]
    )
    let mixer = AudioMixer(name: "check_make_mixer_muted", config: config, chunkSize: 2048)

    XCTAssertEqual(mixer.channelsIn, 2)
    XCTAssertEqual(mixer.channelsOut, 4)

    // Process a non-trivial signal so mute/pass-through is unambiguous
    let input = AudioChunk(waveforms: [
      [1.0, 0.0, 0.0, 0.0],  // channel 0
      [0.0, 1.0, 0.0, 0.0],  // channel 1
    ])
    let output = mixer.process(chunk: input)

    XCTAssertEqual(output.channels, 4)

    // Muted outputs must have empty source lists → silence
    assertSilence(output.waveforms[0])  // dest 0 muted
    assertSilence(output.waveforms[2])  // dest 2 muted

    // Unmuted outputs must pass through at gain 1.0
    XCTAssertEqual(output.waveforms[1][0], 0.0, accuracy: accuracy)
    XCTAssertEqual(output.waveforms[1][1], 1.0, accuracy: accuracy, "out1 gain should be 1.0")
    XCTAssertEqual(output.waveforms[3][0], 0.0, accuracy: accuracy)
    XCTAssertEqual(output.waveforms[3][1], 1.0, accuracy: accuracy, "out3 gain should be 1.0")
  }

  // MARK: - Additional edge-case tests

  /// Verify that validFrames is propagated correctly from input to output.
  func testMixerValidFramesPropagation() {
    let config = MixerConfig(
      channelsIn: 1,
      channelsOut: 1,
      mapping: [
        MixerMapping(dest: 0, sources: [MixerSource(channel: 0, gain: 0.0)])
      ]
    )
    let mixer = AudioMixer(name: "valid_frames", config: config, chunkSize: 2048)

    var input = AudioChunk(frames: 16, channels: 1)
    input.validFrames = 10  // only 10 of 16 frames are valid

    let output = mixer.process(chunk: input)
    XCTAssertEqual(output.validFrames, 10)
  }

  /// An output channel with no mapping entry should produce silence.
  func testMixerUnmappedOutputIsSilent() {
    // channelsOut = 2 but only dest 0 is mapped; dest 1 has no entry.
    let config = MixerConfig(
      channelsIn: 1,
      channelsOut: 2,
      mapping: [
        MixerMapping(dest: 0, sources: [MixerSource(channel: 0, gain: 0.0)])
        // dest 1 intentionally omitted
      ]
    )
    let mixer = AudioMixer(name: "unmapped_output", config: config, chunkSize: 2048)

    let input = AudioChunk(waveforms: [[1.0, 1.0, 1.0, 1.0]])
    let output = mixer.process(chunk: input)

    XCTAssertEqual(output.channels, 2)
    assertAllSamples(output.waveforms[0], equal: 1.0, accuracy: accuracy)
    assertSilence(output.waveforms[1])
  }

  // MARK: - process(input:into:) — zero-allocation API

  /// `process(input:into:)` must produce bit-identical output to `process(chunk:)`
  /// for the same input and config.
  func testMixerInoutAPI_MatchesAllocatingAPI() {
    let config = MixerConfig(
      channelsIn: 2,
      channelsOut: 3,
      mapping: [
        MixerMapping(
          dest: 0,
          sources: [
            MixerSource(channel: 0, gain: -3.0),  // dB
            MixerSource(channel: 1, gain: -6.0),
          ]),
        MixerMapping(dest: 1, sources: [MixerSource(channel: 1, gain: 0.0)]),
        MixerMapping(
          dest: 2, sources: [MixerSource(channel: 0, gain: 0.0)], mute: true),  // silent output
      ]
    )
    let mixerA = AudioMixer(name: "in1", config: config, chunkSize: 2048)
    let mixerB = AudioMixer(name: "in2", config: config, chunkSize: 2048)

    // Random-ish input, two channels of 1024 samples each.
    var rng = SystemRandomNumberGenerator()
    let waveforms = (0..<2).map { _ in
      (0..<1024).map { _ in Double.random(in: -1.0...1.0, using: &rng) }
    }
    let input = AudioChunk(waveforms: waveforms, validFrames: 1024)
    let outAlloc = mixerA.process(chunk: input)

    // Pre-allocate output for the inout API. Note: 3 channels (matches channelsOut),
    // each capable of holding `validFrames` samples.
    var preallocated = AudioChunk(
      waveforms: [[PrcFmt]](repeating: [PrcFmt](repeating: 0, count: 1024), count: 3),
      validFrames: 0)
    try! mixerB.process(input: input, into: &preallocated)

    XCTAssertEqual(outAlloc.validFrames, preallocated.validFrames)
    XCTAssertEqual(preallocated.validFrames, 1024)
    for ch in 0..<3 {
      for i in 0..<1024 {
        XCTAssertEqual(
          outAlloc.waveforms[ch][i], preallocated.waveforms[ch][i],
          accuracy: 1e-12, "ch \(ch) sample \(i)")
      }
    }
  }

  /// Calling `process(input:into:)` repeatedly should overwrite the output cleanly
  /// — no residual data from prior calls leaking in (the mixer accumulates into
  /// the output, so the prefix must be zeroed first).
  func testMixerInoutAPI_OverwritesPriorData() {
    let config = MixerConfig(
      channelsIn: 1,
      channelsOut: 1,
      mapping: [MixerMapping(dest: 0, sources: [MixerSource(channel: 0, gain: 0.0)])]
    )
    let mixer = AudioMixer(name: "passthrough", config: config, chunkSize: 2048)

    var output = AudioChunk(
      waveforms: [Array(repeating: 99.0, count: 16)],  // garbage prefill
      validFrames: 0)

    let input = AudioChunk(waveforms: [[1.0, 2.0, 3.0, 4.0]], validFrames: 4)
    try! mixer.process(input: input, into: &output)

    XCTAssertEqual(output.validFrames, 4)
    XCTAssertEqual(output.waveforms[0][0], 1.0, accuracy: 1e-12)
    XCTAssertEqual(output.waveforms[0][1], 2.0, accuracy: 1e-12)
    XCTAssertEqual(output.waveforms[0][2], 3.0, accuracy: 1e-12)
    XCTAssertEqual(output.waveforms[0][3], 4.0, accuracy: 1e-12)
    // Garbage in slots 4..15 is fine — caller honours validFrames.
  }

  func testMixerInoutAPI_RejectsTooSmallOutputBuffer() {
    let config = MixerConfig(
      channelsIn: 1,
      channelsOut: 1,
      mapping: [MixerMapping(dest: 0, sources: [MixerSource(channel: 0, gain: 0.0)])]
    )
    let mixer = AudioMixer(name: "too-small", config: config, chunkSize: 2048)

    let input = AudioChunk(waveforms: [Array(repeating: 1.0, count: 256)], validFrames: 256)
    var output = AudioChunk(waveforms: [Array(repeating: 0.0, count: 16)], validFrames: 0)

    do {
      try mixer.process(input: input, into: &output)
      XCTFail("Expected outputBufferTooSmall")
    } catch MixerError.outputBufferTooSmall(let needed, let got) {
      XCTAssertEqual(needed, 256)
      XCTAssertEqual(got, 16)
    } catch {
      XCTFail("Unexpected error: \(error)")
    }
  }

  func testMixerInoutAPI_RejectsChannelMismatch() {
    let config = MixerConfig(
      channelsIn: 1, channelsOut: 2,
      mapping: [MixerMapping(dest: 0, sources: [MixerSource(channel: 0, gain: 0.0)])]
    )
    let mixer = AudioMixer(name: "ch-mismatch", config: config, chunkSize: 2048)
    let input = AudioChunk(waveforms: [[1.0, 2.0, 3.0]], validFrames: 3)
    // Output has 1 channel but mixer expects to write 2.
    var output = AudioChunk(waveforms: [Array(repeating: 0.0, count: 8)], validFrames: 0)

    do {
      try mixer.process(input: input, into: &output)
      XCTFail("Expected channelCountMismatch")
    } catch MixerError.channelCountMismatch(let needed, let got) {
      XCTAssertEqual(needed, 2)
      XCTAssertEqual(got, 1)
    } catch {
      XCTFail("Unexpected error: \(error)")
    }
  }
}
