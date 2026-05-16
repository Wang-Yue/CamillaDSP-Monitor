import Foundation
import Testing

@testable import DSPAudio
@testable import DSPConfig
@testable import DSPPipeline

@Suite struct ConfigurationTests {

  @Test func ParseValidConfig() throws {
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
    #expect(config.devices.samplerate == 44100)
    #expect(config.devices.chunksize == 1024)
    #expect(config.devices.capture.channels == 2)
    #expect(config.devices.playback.channels == 2)
  }

  @Test func ParseInvalidJSON() {
    let json = """
      {
          "devices": {
              "samplerate": 44100,
              "chunksize": 1024,
              "capture": {
                  "type": "CoreAudio",
                  "channels": 2
      """  // Missing closing braces
    do {
      _ = try ConfigLoader.parse(json: json)
      Issue.record("Expected error to be thrown")
    } catch {
      if case ConfigError.parseError = error {
        // OK
      } else {
        Issue.record("Expected parseError, got \(error)")
      }

    }
  }

  @Test func ValidateSampleRate() {
    let config = DSPConfiguration(
      devices: DevicesConfig(
        samplerate: 0, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    do {
      _ = try ConfigLoader.validate(config)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.validationError(let msg) = error else {
        Issue.record("Expected validationError, got \(error)")
        return
      }
      #expect(msg.contains("Sample rate must be positive"))

    }
  }

  @Test func ValidateChunkSize() {
    let config = DSPConfiguration(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 0,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    do {
      _ = try ConfigLoader.validate(config)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.validationError(let msg) = error else {
        Issue.record("Expected validationError, got \(error)")
        return
      }
      #expect(msg.contains("Chunk size must be positive"))

    }
  }

  @Test func ValidateChannels() {
    var config = DSPConfiguration(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 0),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    do {
      _ = try ConfigLoader.validate(config)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.validationError(let msg) = error else {
        Issue.record("Expected validationError, got \(error)")
        return
      }
      #expect(msg.contains("Capture channels must be positive"))

    }

    config = DSPConfiguration(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 0)))
    do {
      _ = try ConfigLoader.validate(config)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.validationError(let msg) = error else {
        Issue.record("Expected validationError, got \(error)")
        return
      }
      #expect(msg.contains("Playback channels must be positive"))

    }
  }

  @Test func ValidatePipelineFilterMissingNames() {
    let step = PipelineStep(type: .filter, channel: 0)
    let config = DSPConfiguration(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.pipeline = [step]
    do {
      _ = try ConfigLoader.validate(fullConfig)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.invalidPipeline(let msg) = error else {
        Issue.record("Expected invalidPipeline, got \(error)")
        return
      }
      #expect(msg.contains("must have 'names'"))

    }
  }

  @Test func ValidatePipelineFilterMissingChannels() {
    let step = PipelineStep(type: .filter, names: ["myfilter"])
    let config = DSPConfiguration(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.pipeline = [step]
    do {
      _ = try ConfigLoader.validate(fullConfig)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.invalidPipeline(let msg) = error else {
        Issue.record("Expected invalidPipeline, got \(error)")
        return
      }
      #expect(msg.contains("must have 'channel' or 'channels'"))

    }
  }

  @Test func ValidatePipelineFilterUndefined() {
    let step = PipelineStep(type: .filter, channel: 0, names: ["undefined_filter"])
    let config = DSPConfiguration(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.pipeline = [step]
    do {
      _ = try ConfigLoader.validate(fullConfig)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.invalidPipeline(let msg) = error else {
        Issue.record("Expected invalidPipeline, got \(error)")
        return
      }
      #expect(msg.contains("referenced in pipeline but not defined"))

    }
  }

  @Test func ValidatePipelineFilterChannelOutOfRange() {
    let filter = FilterConfig.gain(GainParameters())
    let step = PipelineStep(type: .filter, channel: 2, names: ["myfilter"])  // Only 2 channels (0, 1)
    let config = DSPConfiguration(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.filters = ["myfilter": filter]
    fullConfig.pipeline = [step]
    do {
      _ = try ConfigLoader.validate(fullConfig)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.invalidPipeline(let msg) = error else {
        Issue.record("Expected invalidPipeline, got \(error)")
        return
      }
      #expect(msg.contains("references channel 2 but pipeline only has 2"))

    }
  }

  @Test func ValidatePipelineMixerMissingName() {
    let step = PipelineStep(type: .mixer)
    let config = DSPConfiguration(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.pipeline = [step]
    do {
      _ = try ConfigLoader.validate(fullConfig)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.invalidPipeline(let msg) = error else {
        Issue.record("Expected invalidPipeline, got \(error)")
        return
      }
      #expect(msg.contains("must have 'name'"))

    }
  }

  @Test func ValidatePipelineMixerUndefined() {
    let step = PipelineStep(type: .mixer, name: "undefined_mixer")
    let config = DSPConfiguration(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.pipeline = [step]
    do {
      _ = try ConfigLoader.validate(fullConfig)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.invalidPipeline(let msg) = error else {
        Issue.record("Expected invalidPipeline, got \(error)")
        return
      }
      #expect(msg.contains("referenced in pipeline but not defined"))

    }
  }

  @Test func ValidatePipelineMixerInputMismatch() {
    let mixer = MixerConfig(channelsIn: 3, channelsOut: 2, mapping: [])  // Expects 3, but capture has 2
    let step = PipelineStep(type: .mixer, name: "mymixer")
    let config = DSPConfiguration(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.mixers = ["mymixer": mixer]
    fullConfig.pipeline = [step]
    do {
      _ = try ConfigLoader.validate(fullConfig)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.invalidPipeline(let msg) = error else {
        Issue.record("Expected invalidPipeline, got \(error)")
        return
      }
      #expect(msg.contains("expects 3 input channel(s) but pipeline has 2"))

    }
  }

  @Test func ValidatePipelineOutputMismatch() {
    let mixer = MixerConfig(channelsIn: 2, channelsOut: 3, mapping: [])  // Outputs 3
    let step = PipelineStep(type: .mixer, name: "mymixer")
    let config = DSPConfiguration(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))  // Playback expects 2
    var fullConfig = config
    fullConfig.mixers = ["mymixer": mixer]
    fullConfig.pipeline = [step]
    do {
      _ = try ConfigLoader.validate(fullConfig)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.invalidPipeline(let msg) = error else {
        Issue.record("Expected invalidPipeline, got \(error)")
        return
      }
      #expect(msg.contains("outputs 3 channel(s) but playback device expects 2"))

    }
  }

  @Test func ValidatePipelineBypassedStep() throws {
    let filter = FilterConfig.gain(GainParameters())
    let step = PipelineStep(type: .filter, channel: 2, names: ["myfilter"], bypassed: true)  // Channel 2 is out of range, but step is bypassed!
    let config = DSPConfiguration(
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

  @Test func ConfigErrorDescription() {
    #expect(ConfigError.parseError("test").description == "Parse error: test")
    #expect(ConfigError.validationError("test").description == "Validation error: test")
    #expect(ConfigError.invalidFilter("test").description == "Invalid filter: test")
    #expect(ConfigError.invalidMixer("test").description == "Invalid mixer: test")
    #expect(ConfigError.invalidPipeline("test").description == "Invalid pipeline: test")
  }

  @Test func MixerValidatorDestOutOfRange() {
    let mapping = MixerMapping(dest: 2, sources: [])  // Dest 2 >= channelsOut 2
    let mixer = MixerConfig(channelsIn: 2, channelsOut: 2, mapping: [mapping])
    do {
      try mixer.validate()
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.invalidMixer(let msg) = error else {
        Issue.record("Expected invalidMixer, got \(error)")
        return
      }
      #expect(msg.contains("mixer dest 2 >= channels_out 2"))

    }
  }

  @Test func MixerValidatorDuplicateDest() {
    let mapping1 = MixerMapping(dest: 0, sources: [])
    let mapping2 = MixerMapping(dest: 0, sources: [])
    let mixer = MixerConfig(channelsIn: 2, channelsOut: 2, mapping: [mapping1, mapping2])
    do {
      try mixer.validate()
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.invalidMixer(let msg) = error else {
        Issue.record("Expected invalidMixer, got \(error)")
        return
      }
      #expect(msg.contains("mixer dest 0 mapped more than once"))

    }
  }

  @Test func MixerValidatorSourceOutOfRange() {
    let source = MixerSource(channel: 2)  // Source 2 >= channelsIn 2
    let mapping = MixerMapping(dest: 0, sources: [source])
    let mixer = MixerConfig(channelsIn: 2, channelsOut: 2, mapping: [mapping])
    do {
      try mixer.validate()
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.invalidMixer(let msg) = error else {
        Issue.record("Expected invalidMixer, got \(error)")
        return
      }
      #expect(msg.contains("mixer source channel 2 >= channels_in 2"))

    }
  }

  @Test func MixerValidatorDuplicateSource() {
    let source1 = MixerSource(channel: 0)
    let source2 = MixerSource(channel: 0)
    let mapping = MixerMapping(dest: 0, sources: [source1, source2])
    let mixer = MixerConfig(channelsIn: 2, channelsOut: 2, mapping: [mapping])
    do {
      try mixer.validate()
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.invalidMixer(let msg) = error else {
        Issue.record("Expected invalidMixer, got \(error)")
        return
      }
      #expect(msg.contains("mixer source channel 0 listed more than once for dest 0"))

    }
  }

  @Test func ValidateInvalidFilterConfig() {
    var params = GainParameters()
    params.gain = 200.0  // Invalid (>150)
    let filter = FilterConfig.gain(params)
    let config = DSPConfiguration(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.filters = ["mygain": filter]
    do {
      _ = try ConfigLoader.validate(fullConfig)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.invalidFilter(let msg) = error else {
        Issue.record("Expected invalidFilter, got \(error)")
        return
      }
      #expect(msg.contains("gain must be in (-150, 150)"))

    }
  }

  @Test func ValidateInvalidMixerConfig() {
    let mapping = MixerMapping(dest: 5, sources: [])  // Invalid (dest >= channelsOut)
    let mixer = MixerConfig(channelsIn: 2, channelsOut: 2, mapping: [mapping])
    let config = DSPConfiguration(
      devices: DevicesConfig(
        samplerate: 44100, chunksize: 1024,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 2),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))
    var fullConfig = config
    fullConfig.mixers = ["mymixer": mixer]
    do {
      _ = try ConfigLoader.validate(fullConfig)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case ConfigError.invalidMixer(let msg) = error else {
        Issue.record("Expected invalidMixer, got \(error)")
        return
      }
      #expect(msg.contains("mixer dest 5 >= channels_out 2"))

    }
  }
}
