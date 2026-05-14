import Foundation
import Testing

@testable import DSPAudio
@testable import DSPConfig
@testable import DSPPipeline

@Suite struct PipelineTests {

  @Test func PipelineInitEmpty() throws {
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    let params = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    _ = try Pipeline(config: config, processingParams: params)
  }

  @Test func PipelineProcessPassthrough() throws {
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    let params = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    let pipeline = try Pipeline(config: config, processingParams: params)

    let chunk = AudioChunk(frames: 1024, channels: 2)
    // Fill with sine wave
    for ch in 0..<2 {
      for t in 0..<1024 {
        chunk[ch][t] = sin(2.0 * .pi * 1000.0 * Double(t) / 44100.0)
      }
    }

    var output = AudioChunk(frames: 1024, channels: 2)
    try pipeline.process(input: chunk, into: &output)

    // Volume is 0dB by default, so output should match input (modulo float precision if any)
    #expect(output.validFrames == 1024)
    #expect(output.channels == 2)
  }

  @Test func PipelineWithFilter() throws {
    var config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))

    // Create a gain filter (-6dB)
    var params = GainParameters()
    params.gain = -6.0
    params.scale = .dB
    let filterConfig = FilterConfig.gain(params)
    config.filters = ["mygain": filterConfig]

    // Apply to channel 0
    let step = PipelineStep(type: .filter, channel: 0, names: ["mygain"])
    config.pipeline = [step]

    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    let pipeline = try Pipeline(config: config, processingParams: procParams)

    let chunk = AudioChunk(frames: 1024, channels: 2)
    for ch in 0..<2 {
      for t in 0..<1024 {
        chunk[ch][t] = 1.0
      }
    }

    var output = AudioChunk(frames: 1024, channels: 2)
    try pipeline.process(input: chunk, into: &output)

    // Channel 0 should be attenuated by -6dB (~0.501)
    #expect(abs(output.waveforms[0][0] - PrcFmt.fromDB(-6.0)) <= 1e-5)
    // Channel 1 should be untouched (1.0)
    #expect(abs(output.waveforms[1][0] - 1.0) <= 1e-5)
  }

  @Test func PipelineWithMixer() throws {
    var config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))

    // Create a mixer that swaps channels 0 and 1
    let map0 = MixerMapping(dest: 0, sources: [MixerSource(channel: 1)])
    let map1 = MixerMapping(dest: 1, sources: [MixerSource(channel: 0)])
    let mixerConfig = MixerConfig(channelsIn: 2, channelsOut: 2, mapping: [map0, map1])
    config.mixers = ["swap": mixerConfig]

    let step = PipelineStep(type: .mixer, name: "swap")
    config.pipeline = [step]

    let params = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    let pipeline = try Pipeline(config: config, processingParams: params)

    let chunk = AudioChunk(frames: 1024, channels: 2)
    // Fill channel 0 with 1.0, channel 1 with 2.0
    for t in 0..<1024 {
      chunk[0][t] = 1.0
      chunk[1][t] = 2.0
    }

    var output = AudioChunk(frames: 1024, channels: 2)
    try pipeline.process(input: chunk, into: &output)

    // Channels should be swapped!
    #expect(abs(output.waveforms[0][0] - 2.0) <= 1e-5)
    #expect(abs(output.waveforms[1][0] - 1.0) <= 1e-5)
  }

  @Test func PipelineBypassedFilter() throws {
    var config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))

    var params = GainParameters()
    params.gain = -6.0
    params.scale = .dB
    let filterConfig = FilterConfig.gain(params)
    config.filters = ["mygain": filterConfig]

    // Bypassed!
    let step = PipelineStep(type: .filter, channel: 0, names: ["mygain"], bypassed: true)
    config.pipeline = [step]

    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    let pipeline = try Pipeline(config: config, processingParams: procParams)

    let chunk = AudioChunk(frames: 1024, channels: 2)
    for ch in 0..<2 {
      for t in 0..<1024 {
        chunk[ch][t] = 1.0
      }
    }

    var output = AudioChunk(frames: 1024, channels: 2)
    try pipeline.process(input: chunk, into: &output)

    // Channel 0 should be UNTOUCHED (1.0) because step is bypassed!
    #expect(abs(output.waveforms[0][0] - 1.0) <= 1e-5)
  }

  @Test func PipelineFilterChannelOutOfBounds() throws {
    var config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))

    var params = GainParameters()
    params.gain = -6.0
    params.scale = .dB
    let filterConfig = FilterConfig.gain(params)
    config.filters = ["mygain": filterConfig]

    // We force a step with channel 2 (out of bounds) into the pipeline!
    // Note: ConfigLoader.validate would catch this, but we are testing Pipeline's resilience!
    let step = PipelineStep(type: .filter, channel: 2, names: ["mygain"])
    config.pipeline = [step]

    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    // Pipeline init doesn't validate channel bounds against chunk at runtime, it trusts config.
    let pipeline = try Pipeline(config: config, processingParams: procParams)

    let chunk = AudioChunk(frames: 1024, channels: 2)
    for ch in 0..<2 {
      for t in 0..<1024 {
        chunk[ch][t] = 1.0
      }
    }

    // Should NOT crash, should just skip the out-of-bounds channel!
    var output = AudioChunk(frames: 1024, channels: 2)
    try pipeline.process(input: chunk, into: &output)

    #expect(abs(output.waveforms[0][0] - 1.0) <= 1e-5)
    #expect(abs(output.waveforms[1][0] - 1.0) <= 1e-5)
  }

  @Test func PipelineVolumeChange() throws {
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    let params = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    let pipeline = try Pipeline(config: config, processingParams: params)

    // Set target volume to -10dB
    params.targetVolume = -10.0

    let chunk = AudioChunk(frames: 1024, channels: 2)
    for ch in 0..<2 {
      for t in 0..<1024 {
        chunk[ch][t] = 1.0
      }
    }

    var output = AudioChunk(frames: 1024, channels: 2)
    try pipeline.process(input: chunk, into: &output)

    // Volume is applied immediately for the whole chunk!
    #expect(abs(output.waveforms[0][0] - PrcFmt.fromDB(-10.0)) <= 1e-5)
    #expect(abs(output.waveforms[0][1023] - PrcFmt.fromDB(-10.0)) <= 1e-5)
  }

  @Test func PipelineMute() throws {
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    let params = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    let pipeline = try Pipeline(config: config, processingParams: params)

    // Mute!
    params.isMuted = true

    let chunk = AudioChunk(frames: 1024, channels: 2)
    for ch in 0..<2 {
      for t in 0..<1024 {
        chunk[ch][t] = 1.0
      }
    }

    var output = AudioChunk(frames: 1024, channels: 2)
    try pipeline.process(input: chunk, into: &output)

    // Output should be zero!
    #expect(abs(output.waveforms[0][0] - 0.0) <= 1e-5)
    #expect(abs(output.waveforms[0][1023] - 0.0) <= 1e-5)
  }

  @Test func PipelineInitFilterMissingNames() {
    let step = PipelineStep(type: .filter, channel: 0)  // names is nil
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.pipeline = [step]
    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    do {
      _ = try Pipeline(config: fullConfig, processingParams: procParams)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.invalidPipeline(let msg) = error else {
        Issue.record("Expected invalidPipeline, got \(error)")
        return
      }
      #expect(msg.contains("Filter step missing names"))

    }
  }

  @Test func PipelineInitFilterChannels() throws {
    var params = GainParameters()
    params.gain = -6.0
    let filter = FilterConfig.gain(params)
    let step = PipelineStep(type: .filter, channels: [0, 1], names: ["mygain"])
    var config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    config.filters = ["mygain": filter]
    config.pipeline = [step]
    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    _ = try Pipeline(config: config, processingParams: procParams)
  }

  @Test func PipelineInitFilterAllChannels() throws {
    var params = GainParameters()
    params.gain = -6.0
    let filter = FilterConfig.gain(params)
    let step = PipelineStep(type: .filter, names: ["mygain"])  // No channel, no channels
    var config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    config.filters = ["mygain": filter]
    config.pipeline = [step]
    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    _ = try Pipeline(config: config, processingParams: procParams)
  }

  @Test func PipelineInitFilterUndefined() {
    let step = PipelineStep(type: .filter, channel: 0, names: ["undefined_filter"])
    var config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    config.pipeline = [step]
    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    do {
      _ = try Pipeline(config: config, processingParams: procParams)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.invalidPipeline(let msg) = error else {
        Issue.record("Expected invalidPipeline, got \(error)")
        return
      }
      #expect(msg.contains("Filter 'undefined_filter' not defined"))

    }
  }

  @Test func PipelineInitMixerMissingName() {
    let step = PipelineStep(type: .mixer)  // name is nil
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.pipeline = [step]
    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    do {
      _ = try Pipeline(config: fullConfig, processingParams: procParams)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.invalidPipeline(let msg) = error else {
        Issue.record("Expected invalidPipeline, got \(error)")
        return
      }
      #expect(msg.contains("Mixer step missing name or config"))

    }
  }

  @Test func PipelineWithLoudnessFilters() throws {
    var config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var loudParams = LoudnessParameters()
    loudParams.referenceLevel = -20.0
    let loudConfig = FilterConfig.loudness(loudParams)

    config.filters = ["myloud": loudConfig]
    let step = PipelineStep(type: .filter, channel: 0, names: ["myloud"])
    config.pipeline = [step]

    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    _ = try Pipeline(config: config, processingParams: procParams)

  }

  @Test func PipelineSequentialMixersZeroAllocationRecovery() throws {
    var config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))

    // 1. Create a 2to4 mixer
    let map2to4 = [
      MixerMapping(dest: 0, sources: [MixerSource(channel: 0)]),
      MixerMapping(dest: 1, sources: [MixerSource(channel: 0)]),
      MixerMapping(dest: 2, sources: [MixerSource(channel: 1)]),
      MixerMapping(dest: 3, sources: [MixerSource(channel: 1)]),
    ]
    let mixer2to4Config = MixerConfig(channelsIn: 2, channelsOut: 4, mapping: map2to4)

    // 2. Create a 4to2 mixer
    let map4to2 = [
      MixerMapping(dest: 0, sources: [MixerSource(channel: 0), MixerSource(channel: 2)]),
      MixerMapping(dest: 1, sources: [MixerSource(channel: 1), MixerSource(channel: 3)]),
    ]
    let mixer4to2Config = MixerConfig(channelsIn: 4, channelsOut: 2, mapping: map4to2)

    config.mixers = ["2to4": mixer2to4Config, "4to2": mixer4to2Config]
    config.pipeline = [
      PipelineStep(type: .mixer, name: "2to4"),
      PipelineStep(type: .mixer, name: "4to2"),
    ]

    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    let pipeline = try Pipeline(config: config, processingParams: procParams)

    // Run multiple audio blocks sequential to ensure self-healing and permutation restores perfect shape
    let chunk = AudioChunk(frames: 1024, channels: 2)
    for t in 0..<1024 {
      chunk[0][t] = 1.0
      chunk[1][t] = 2.0
    }

    // Process Block 1
    var output1 = AudioChunk(frames: 1024, channels: 2)
    try pipeline.process(input: chunk, into: &output1)
    #expect(output1.channels == 2)
    #expect(abs(output1.waveforms[0][0] - 3.0) <= 1e-5)  // Left + Right
    #expect(abs(output1.waveforms[1][0] - 3.0) <= 1e-5)  // Left + Right

    // Process Block 2 (will throw channelCountMismatch if scratch buffers are in wrong slot)
    let chunk2 = AudioChunk(frames: 1024, channels: 2)
    for t in 0..<1024 {
      chunk2[0][t] = 3.0
      chunk2[1][t] = 4.0
    }
    var output2 = AudioChunk(frames: 1024, channels: 2)
    try pipeline.process(input: chunk2, into: &output2)
    #expect(output2.channels == 2)
    #expect(abs(output2.waveforms[0][0] - 7.0) <= 1e-5)
  }

  @Test func PipelineProcessValidationThrows() throws {
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    let pipeline = try Pipeline(config: config, processingParams: procParams)

    // 1. Test inputSizeMismatch
    let tooLargeInput = AudioChunk(frames: 2048, channels: 2)
    var output = AudioChunk(frames: 1024, channels: 2)
    do {
      _ = try pipeline.process(input: tooLargeInput, into: &output)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case PipelineError.inputSizeMismatch(let needed, let got) = error else {
        Issue.record("Expected inputSizeMismatch, got \(error)")
        return
      }
      #expect(needed == 1024)
      #expect(got == 2048)

    }

    // 2. Test input channel Count mismatch
    let wrongInputChannels = AudioChunk(frames: 1024, channels: 1)
    do {
      _ = try pipeline.process(input: wrongInputChannels, into: &output)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case PipelineError.channelCountMismatch(let needed, let got) = error else {
        Issue.record("Expected channelCountMismatch, got \(error)")
        return
      }
      #expect(needed == 2)
      #expect(got == 1)

    }

    // 3. Test output channel Count mismatch
    let input = AudioChunk(frames: 1024, channels: 2)
    var wrongOutputChannels = AudioChunk(frames: 1024, channels: 3)
    do {
      _ = try pipeline.process(input: input, into: &wrongOutputChannels)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case PipelineError.channelCountMismatch(let needed, let got) = error else {
        Issue.record("Expected channelCountMismatch, got \(error)")
        return
      }
      #expect(needed == 2)
      #expect(got == 3)

    }

    // 4. Test output capacity too small
    var tooSmallOutput = AudioChunk(waveforms: [
      [PrcFmt](repeating: 0, count: 512), [PrcFmt](repeating: 0, count: 512),
    ])
    do {
      _ = try pipeline.process(input: input, into: &tooSmallOutput)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case PipelineError.outputBufferTooSmall(let needed, let got) = error else {
        Issue.record("Expected outputBufferTooSmall, got \(error)")
        return
      }
      #expect(needed == 1024)
      #expect(got == 512)

    }
  }
}
