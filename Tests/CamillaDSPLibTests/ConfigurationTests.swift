import XCTest

@testable import CamillaDSPLib

final class ConfigurationTests: XCTestCase {

  func testParseValidConfig() throws {
    let json = """
      {
          "devices": {
              "samplerate": 44100,
              "chunksize": 1024,
              "capture": {
                  "type": "CoreAudio",
                  "channels": 2
              },
              "playback": {
                  "type": "CoreAudio",
                  "channels": 2
              }
          }
      }
      """
    let config = try ConfigLoader.parse(json: json)
    XCTAssertEqual(config.devices.samplerate, 44100)
    XCTAssertEqual(config.devices.chunksize, 1024)
    XCTAssertEqual(config.devices.capture.channels, 2)
    XCTAssertEqual(config.devices.playback.channels, 2)
  }

  func testParseInvalidJSON() {
    let json = """
      {
          "devices": {
              "samplerate": 44100,
              "chunksize": 1024,
              "capture": {
                  "type": "CoreAudio",
                  "channels": 2
      """  // Missing closing braces
    XCTAssertThrowsError(try ConfigLoader.parse(json: json)) { error in
      if case ConfigError.parseError = error {
        // OK
      } else {
        XCTFail("Expected parseError, got \(error)")
      }
    }
  }

  func testValidateSampleRate() {
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 0, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    XCTAssertThrowsError(try ConfigLoader.validate(config)) { error in
      guard case ConfigError.validationError(let msg) = error else {
        return XCTFail("Expected validationError, got \(error)")
      }
      XCTAssertTrue(msg.contains("Sample rate must be positive"))
    }
  }

  func testValidateChunkSize() {
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 0,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    XCTAssertThrowsError(try ConfigLoader.validate(config)) { error in
      guard case ConfigError.validationError(let msg) = error else {
        return XCTFail("Expected validationError, got \(error)")
      }
      XCTAssertTrue(msg.contains("Chunk size must be positive"))
    }
  }

  func testValidateChannels() {
    var config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 0),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    XCTAssertThrowsError(try ConfigLoader.validate(config)) { error in
      guard case ConfigError.validationError(let msg) = error else {
        return XCTFail("Expected validationError, got \(error)")
      }
      XCTAssertTrue(msg.contains("Capture channels must be positive"))
    }

    config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 0)))
    XCTAssertThrowsError(try ConfigLoader.validate(config)) { error in
      guard case ConfigError.validationError(let msg) = error else {
        return XCTFail("Expected validationError, got \(error)")
      }
      XCTAssertTrue(msg.contains("Playback channels must be positive"))
    }
  }

  func testValidatePipelineFilterMissingNames() {
    let step = PipelineStep(type: .filter, channel: 0)
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.pipeline = [step]
    XCTAssertThrowsError(try ConfigLoader.validate(fullConfig)) { error in
      guard case ConfigError.invalidPipeline(let msg) = error else {
        return XCTFail("Expected invalidPipeline, got \(error)")
      }
      XCTAssertTrue(msg.contains("must have 'names'"))
    }
  }

  func testValidatePipelineFilterMissingChannels() {
    let step = PipelineStep(type: .filter, names: ["myfilter"])
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.pipeline = [step]
    XCTAssertThrowsError(try ConfigLoader.validate(fullConfig)) { error in
      guard case ConfigError.invalidPipeline(let msg) = error else {
        return XCTFail("Expected invalidPipeline, got \(error)")
      }
      XCTAssertTrue(msg.contains("must have 'channel' or 'channels'"))
    }
  }

  func testValidatePipelineFilterUndefined() {
    let step = PipelineStep(type: .filter, channel: 0, names: ["undefined_filter"])
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.pipeline = [step]
    XCTAssertThrowsError(try ConfigLoader.validate(fullConfig)) { error in
      guard case ConfigError.invalidPipeline(let msg) = error else {
        return XCTFail("Expected invalidPipeline, got \(error)")
      }
      XCTAssertTrue(msg.contains("referenced in pipeline but not defined"))
    }
  }

  func testValidatePipelineFilterChannelOutOfRange() {
    let filter = FilterConfig(type: .gain, parameters: FilterParameters())
    let step = PipelineStep(type: .filter, channel: 2, names: ["myfilter"])  // Only 2 channels (0, 1)
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.filters = ["myfilter": filter]
    fullConfig.pipeline = [step]
    XCTAssertThrowsError(try ConfigLoader.validate(fullConfig)) { error in
      guard case ConfigError.invalidPipeline(let msg) = error else {
        return XCTFail("Expected invalidPipeline, got \(error)")
      }
      XCTAssertTrue(msg.contains("references channel 2 but pipeline only has 2"))
    }
  }

  func testValidatePipelineMixerMissingName() {
    let step = PipelineStep(type: .mixer)
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.pipeline = [step]
    XCTAssertThrowsError(try ConfigLoader.validate(fullConfig)) { error in
      guard case ConfigError.invalidPipeline(let msg) = error else {
        return XCTFail("Expected invalidPipeline, got \(error)")
      }
      XCTAssertTrue(msg.contains("must have 'name'"))
    }
  }

  func testValidatePipelineMixerUndefined() {
    let step = PipelineStep(type: .mixer, name: "undefined_mixer")
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.pipeline = [step]
    XCTAssertThrowsError(try ConfigLoader.validate(fullConfig)) { error in
      guard case ConfigError.invalidPipeline(let msg) = error else {
        return XCTFail("Expected invalidPipeline, got \(error)")
      }
      XCTAssertTrue(msg.contains("referenced in pipeline but not defined"))
    }
  }

  func testValidatePipelineMixerInputMismatch() {
    let mixer = MixerConfig(channelsIn: 3, channelsOut: 2, mapping: [])  // Expects 3, but capture has 2
    let step = PipelineStep(type: .mixer, name: "mymixer")
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.mixers = ["mymixer": mixer]
    fullConfig.pipeline = [step]
    XCTAssertThrowsError(try ConfigLoader.validate(fullConfig)) { error in
      guard case ConfigError.invalidPipeline(let msg) = error else {
        return XCTFail("Expected invalidPipeline, got \(error)")
      }
      XCTAssertTrue(msg.contains("expects 3 input channel(s) but pipeline has 2"))
    }
  }

  func testValidatePipelineOutputMismatch() {
    let mixer = MixerConfig(channelsIn: 2, channelsOut: 3, mapping: [])  // Outputs 3
    let step = PipelineStep(type: .mixer, name: "mymixer")
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))  // Playback expects 2
    var fullConfig = config
    fullConfig.mixers = ["mymixer": mixer]
    fullConfig.pipeline = [step]
    XCTAssertThrowsError(try ConfigLoader.validate(fullConfig)) { error in
      guard case ConfigError.invalidPipeline(let msg) = error else {
        return XCTFail("Expected invalidPipeline, got \(error)")
      }
      XCTAssertTrue(msg.contains("outputs 3 channel(s) but playback device expects 2"))
    }
  }

  func testValidatePipelineBypassedStep() throws {
    let filter = FilterConfig(type: .gain, parameters: FilterParameters())
    let step = PipelineStep(type: .filter, channel: 2, names: ["myfilter"], bypassed: true)  // Channel 2 is out of range, but step is bypassed!
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.filters = ["myfilter": filter]
    fullConfig.pipeline = [step]
    // Should NOT throw because the step is bypassed!
    try ConfigLoader.validate(fullConfig)
  }

  func testConfigErrorDescription() {
    XCTAssertEqual(ConfigError.parseError("test").description, "Parse error: test")
    XCTAssertEqual(ConfigError.validationError("test").description, "Validation error: test")
    XCTAssertEqual(ConfigError.invalidFilter("test").description, "Invalid filter: test")
    XCTAssertEqual(ConfigError.invalidMixer("test").description, "Invalid mixer: test")
    XCTAssertEqual(ConfigError.invalidPipeline("test").description, "Invalid pipeline: test")
  }

  func testMixerValidatorDestOutOfRange() {
    let mapping = MixerMapping(dest: 2, sources: [])  // Dest 2 >= channelsOut 2
    let mixer = MixerConfig(channelsIn: 2, channelsOut: 2, mapping: [mapping])
    XCTAssertThrowsError(try MixerValidator.validate(mixer)) { error in
      // We know it throws invalidFilter due to the bug, but let's check the message!
      guard case ConfigError.invalidFilter(let msg) = error else {
        return XCTFail("Expected invalidFilter (due to bug), got \(error)")
      }
      XCTAssertTrue(msg.contains("mixer dest 2 >= channels_out 2"))
    }
  }

  func testMixerValidatorDuplicateDest() {
    let mapping1 = MixerMapping(dest: 0, sources: [])
    let mapping2 = MixerMapping(dest: 0, sources: [])
    let mixer = MixerConfig(channelsIn: 2, channelsOut: 2, mapping: [mapping1, mapping2])
    XCTAssertThrowsError(try MixerValidator.validate(mixer)) { error in
      guard case ConfigError.invalidFilter(let msg) = error else {
        return XCTFail("Expected invalidFilter, got \(error)")
      }
      XCTAssertTrue(msg.contains("mixer dest 0 mapped more than once"))
    }
  }

  func testMixerValidatorSourceOutOfRange() {
    let source = MixerSource(channel: 2)  // Source 2 >= channelsIn 2
    let mapping = MixerMapping(dest: 0, sources: [source])
    let mixer = MixerConfig(channelsIn: 2, channelsOut: 2, mapping: [mapping])
    XCTAssertThrowsError(try MixerValidator.validate(mixer)) { error in
      guard case ConfigError.invalidFilter(let msg) = error else {
        return XCTFail("Expected invalidFilter, got \(error)")
      }
      XCTAssertTrue(msg.contains("mixer source channel 2 >= channels_in 2"))
    }
  }

  func testMixerValidatorDuplicateSource() {
    let source1 = MixerSource(channel: 0)
    let source2 = MixerSource(channel: 0)
    let mapping = MixerMapping(dest: 0, sources: [source1, source2])
    let mixer = MixerConfig(channelsIn: 2, channelsOut: 2, mapping: [mapping])
    XCTAssertThrowsError(try MixerValidator.validate(mixer)) { error in
      guard case ConfigError.invalidFilter(let msg) = error else {
        return XCTFail("Expected invalidFilter, got \(error)")
      }
      XCTAssertTrue(msg.contains("mixer source channel 0 listed more than once for dest 0"))
    }
  }

  func testValidateInvalidFilterConfig() {
    var params = FilterParameters()
    params.gain = 200.0  // Invalid (>150)
    let filter = FilterConfig(type: .gain, parameters: params)
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.filters = ["mygain": filter]
    XCTAssertThrowsError(try ConfigLoader.validate(fullConfig)) { error in
      guard case ConfigError.invalidFilter(let msg) = error else {
        return XCTFail("Expected invalidFilter, got \(error)")
      }
      XCTAssertTrue(msg.contains("gain must be in (-150, 150)"))
    }
  }

  func testValidateInvalidMixerConfig() {
    let mapping = MixerMapping(dest: 5, sources: [])  // Invalid (dest >= channelsOut)
    let mixer = MixerConfig(channelsIn: 2, channelsOut: 2, mapping: [mapping])
    let config = CamillaDSPConfig(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.mixers = ["mymixer": mixer]
    XCTAssertThrowsError(try ConfigLoader.validate(fullConfig)) { error in
      guard case ConfigError.invalidMixer(let msg) = error else {
        return XCTFail("Expected invalidMixer, got \(error)")
      }
      XCTAssertTrue(msg.contains("mixer dest 5 >= channels_out 2"))
    }
  }
}
