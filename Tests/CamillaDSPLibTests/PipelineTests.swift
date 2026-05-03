import XCTest

@testable import CamillaDSPLib

final class PipelineTests: XCTestCase {

  func testPipelineInitEmpty() throws {
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    let params = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    let pipeline = try Pipeline(config: config, processingParams: params)
    XCTAssertNotNil(pipeline)
  }

  func testPipelineProcessPassthrough() throws {
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    let params = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    let pipeline = try Pipeline(config: config, processingParams: params)

    var chunk = AudioChunk(frames: 1024, channels: 2)
    // Fill with sine wave
    for ch in 0..<2 {
      for t in 0..<1024 {
        chunk.waveforms[ch][t] = sin(2.0 * .pi * 1000.0 * Double(t) / 44100.0)
      }
    }

    try pipeline.process(chunk: &chunk)

    // Volume is 0dB by default, so output should match input (modulo float precision if any)
    XCTAssertEqual(chunk.validFrames, 1024)
    XCTAssertEqual(chunk.channels, 2)
  }

  func testPipelineWithFilter() throws {
    var config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))

    // Create a gain filter (-6dB)
    var params = FilterParameters()
    params.gain = -6.0
    params.scale = .dB
    let filterConfig = FilterConfig(type: .gain, parameters: params)
    config.filters = ["mygain": filterConfig]

    // Apply to channel 0
    let step = PipelineStep(type: .filter, channel: 0, names: ["mygain"])
    config.pipeline = [step]

    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    let pipeline = try Pipeline(config: config, processingParams: procParams)

    var chunk = AudioChunk(frames: 1024, channels: 2)
    for ch in 0..<2 {
      for t in 0..<1024 {
        chunk.waveforms[ch][t] = 1.0
      }
    }

    try pipeline.process(chunk: &chunk)

    // Channel 0 should be attenuated by -6dB (~0.501)
    XCTAssertEqual(chunk.waveforms[0][0], PrcFmt.fromDB(-6.0), accuracy: 1e-5)
    // Channel 1 should be untouched (1.0)
    XCTAssertEqual(chunk.waveforms[1][0], 1.0, accuracy: 1e-5)
  }

  func testPipelineWithMixer() throws {
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

    var chunk = AudioChunk(frames: 1024, channels: 2)
    // Fill channel 0 with 1.0, channel 1 with 2.0
    for t in 0..<1024 {
      chunk.waveforms[0][t] = 1.0
      chunk.waveforms[1][t] = 2.0
    }

    try pipeline.process(chunk: &chunk)

    // Channels should be swapped!
    XCTAssertEqual(chunk.waveforms[0][0], 2.0, accuracy: 1e-5)
    XCTAssertEqual(chunk.waveforms[1][0], 1.0, accuracy: 1e-5)
  }

  func testPipelineBypassedFilter() throws {
    var config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))

    var params = FilterParameters()
    params.gain = -6.0
    params.scale = .dB
    let filterConfig = FilterConfig(type: .gain, parameters: params)
    config.filters = ["mygain": filterConfig]

    // Bypassed!
    let step = PipelineStep(type: .filter, channel: 0, names: ["mygain"], bypassed: true)
    config.pipeline = [step]

    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    let pipeline = try Pipeline(config: config, processingParams: procParams)

    var chunk = AudioChunk(frames: 1024, channels: 2)
    for ch in 0..<2 {
      for t in 0..<1024 {
        chunk.waveforms[ch][t] = 1.0
      }
    }

    try pipeline.process(chunk: &chunk)

    // Channel 0 should be UNTOUCHED (1.0) because step is bypassed!
    XCTAssertEqual(chunk.waveforms[0][0], 1.0, accuracy: 1e-5)
  }

  func testPipelineFilterChannelOutOfBounds() throws {
    var config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))

    var params = FilterParameters()
    params.gain = -6.0
    params.scale = .dB
    let filterConfig = FilterConfig(type: .gain, parameters: params)
    config.filters = ["mygain": filterConfig]

    // We force a step with channel 2 (out of bounds) into the pipeline!
    // Note: ConfigLoader.validate would catch this, but we are testing Pipeline's resilience!
    let step = PipelineStep(type: .filter, channel: 2, names: ["mygain"])
    config.pipeline = [step]

    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    // Pipeline init doesn't validate channel bounds against chunk at runtime, it trusts config.
    let pipeline = try Pipeline(config: config, processingParams: procParams)

    var chunk = AudioChunk(frames: 1024, channels: 2)
    for ch in 0..<2 {
      for t in 0..<1024 {
        chunk.waveforms[ch][t] = 1.0
      }
    }

    // Should NOT crash, should just skip the out-of-bounds channel!
    try pipeline.process(chunk: &chunk)

    XCTAssertEqual(chunk.waveforms[0][0], 1.0, accuracy: 1e-5)
    XCTAssertEqual(chunk.waveforms[1][0], 1.0, accuracy: 1e-5)
  }

  func testPipelineVolumeChange() throws {
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    let params = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    let pipeline = try Pipeline(config: config, processingParams: params)

    // Set target volume to -10dB
    params.targetVolume = -10.0

    var chunk = AudioChunk(frames: 1024, channels: 2)
    for ch in 0..<2 {
      for t in 0..<1024 {
        chunk.waveforms[ch][t] = 1.0
      }
    }

    try pipeline.process(chunk: &chunk)

    // Volume is applied immediately for the whole chunk!
    XCTAssertEqual(chunk.waveforms[0][0], PrcFmt.fromDB(-10.0), accuracy: 1e-5)
    XCTAssertEqual(chunk.waveforms[0][1023], PrcFmt.fromDB(-10.0), accuracy: 1e-5)
  }

  func testPipelineMute() throws {
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    let params = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    let pipeline = try Pipeline(config: config, processingParams: params)

    // Mute!
    params.isMuted = true

    var chunk = AudioChunk(frames: 1024, channels: 2)
    for ch in 0..<2 {
      for t in 0..<1024 {
        chunk.waveforms[ch][t] = 1.0
      }
    }

    try pipeline.process(chunk: &chunk)

    // Output should be zero!
    XCTAssertEqual(chunk.waveforms[0][0], 0.0, accuracy: 1e-5)
    XCTAssertEqual(chunk.waveforms[0][1023], 0.0, accuracy: 1e-5)
  }

  func testPipelineInitFilterMissingNames() {
    let step = PipelineStep(type: .filter, channel: 0)  // names is nil
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.pipeline = [step]
    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    XCTAssertThrowsError(try Pipeline(config: fullConfig, processingParams: procParams)) { error in
      guard case ConfigError.invalidPipeline(let msg) = error else {
        return XCTFail("Expected invalidPipeline, got \(error)")
      }
      XCTAssertTrue(msg.contains("Filter step missing names"))
    }
  }

  func testPipelineInitFilterChannels() throws {
    var params = FilterParameters()
    params.gain = -6.0
    let filter = FilterConfig(type: .gain, parameters: params)
    let step = PipelineStep(type: .filter, channels: [0, 1], names: ["mygain"])
    var config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    config.filters = ["mygain": filter]
    config.pipeline = [step]
    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    let pipeline = try Pipeline(config: config, processingParams: procParams)
    XCTAssertNotNil(pipeline)
  }

  func testPipelineInitFilterAllChannels() throws {
    var params = FilterParameters()
    params.gain = -6.0
    let filter = FilterConfig(type: .gain, parameters: params)
    let step = PipelineStep(type: .filter, names: ["mygain"])  // No channel, no channels
    var config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    config.filters = ["mygain": filter]
    config.pipeline = [step]
    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    let pipeline = try Pipeline(config: config, processingParams: procParams)
    XCTAssertNotNil(pipeline)
  }

  func testPipelineInitFilterUndefined() {
    let step = PipelineStep(type: .filter, channel: 0, names: ["undefined_filter"])
    var config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    config.pipeline = [step]
    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    XCTAssertThrowsError(try Pipeline(config: config, processingParams: procParams)) { error in
      guard case ConfigError.invalidPipeline(let msg) = error else {
        return XCTFail("Expected invalidPipeline, got \(error)")
      }
      XCTAssertTrue(msg.contains("Filter 'undefined_filter' not defined"))
    }
  }

  func testPipelineInitMixerMissingName() {
    let step = PipelineStep(type: .mixer)  // name is nil
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.pipeline = [step]
    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    XCTAssertThrowsError(try Pipeline(config: fullConfig, processingParams: procParams)) { error in
      guard case ConfigError.invalidPipeline(let msg) = error else {
        return XCTFail("Expected invalidPipeline, got \(error)")
      }
      XCTAssertTrue(msg.contains("Mixer step missing name or config"))
    }
  }

  func testPipelineWithVolumeAndLoudnessFilters() throws {
    var config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))

    var volParams = FilterParameters()
    volParams.limit = 10.0
    let volConfig = FilterConfig(type: .volume, parameters: volParams)

    var loudParams = FilterParameters()
    loudParams.referenceLevel = -20.0
    let loudConfig = FilterConfig(type: .loudness, parameters: loudParams)

    config.filters = ["myvol": volConfig, "myloud": loudConfig]
    let step = PipelineStep(type: .filter, channel: 0, names: ["myvol", "myloud"])
    config.pipeline = [step]

    let procParams = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    let pipeline = try Pipeline(config: config, processingParams: procParams)

    XCTAssertNotNil(pipeline)
  }
}
