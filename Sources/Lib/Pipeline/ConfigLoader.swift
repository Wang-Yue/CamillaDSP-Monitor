// JSON loader and cross-component validator for `DSPConfiguration`.
//
// Per-domain validation (filter parameters, mixer mappings) lives next
// to the validated types — see `BiquadParameters.validate(sampleRate:)`,
// `GainParameters.validate()`, `LoudnessParameters.validate()`, and
// `MixerConfig.validate()`. This file owns only:
//   1. JSON → `DSPConfiguration` decoding.
//   2. Top-level field checks (samplerate, chunksize, channel counts).
//   3. The pipeline walk that ties filters/mixers to the device channel
//      counts.
//
// DSPMonitor only ever sends JSON over the actor's
// `start(configJson:)` boundary, so the loader is JSON-only; the
// upstream YAML pathway and Yams dependency have been pruned.

import DSPConfig
import DSPLogging
import Foundation

public enum ConfigLoader {
  private static let logger = Logger(label: "dsp.config")

  /// Parse a DSP configuration from JSON and run full validation.
  public static func parse(json: String) throws -> DSPConfiguration {
    guard let data = json.data(using: .utf8) else {
      throw ConfigError.parseError("JSON config is not valid UTF-8")
    }
    let config: DSPConfiguration
    do {
      config = try JSONDecoder().decode(DSPConfiguration.self, from: data)
    } catch let error as DecodingError {
      throw ConfigError.parseError("\(error)")
    }
    try validate(config)
    return config
  }

  /// Validate a parsed configuration. Top-level field checks first,
  /// then per-component validation, then the pipeline walk.
  public static func validate(_ config: DSPConfiguration) throws {
    try validateTopLevelFields(config)

    if let filters = config.filters {
      for (name, filterConfig) in filters {
        do {
          try filterConfig.validate()
        } catch {
          throw ConfigError.invalidFilter("Filter '\(name)': \(error)")
        }
      }
    }

    if let mixers = config.mixers {
      for (name, mixerConfig) in mixers {
        do {
          try mixerConfig.validate()
        } catch {
          throw ConfigError.invalidMixer("Mixer '\(name)': \(error)")
        }
      }
    }

    try validatePipeline(config)

    logger.info("Configuration validated successfully")
  }

  private static func validateTopLevelFields(_ config: DSPConfiguration) throws {
    guard config.devices.samplerate > 0 else {
      throw ConfigError.validationError("Sample rate must be positive")
    }
    guard config.devices.chunksize > 0 else {
      throw ConfigError.validationError("Chunk size must be positive")
    }
    guard config.devices.capture.channels > 0 else {
      throw ConfigError.validationError("Capture channels must be positive")
    }
    guard config.devices.playback.channels > 0 else {
      throw ConfigError.validationError("Playback channels must be positive")
    }
  }

  /// Walk the pipeline tracking the channel count through each step.
  /// Mirrors the logic in the Rust `config::utils::validate_config`
  /// pipeline walk:
  ///   - Filter step: all channel indices must be < current count;
  ///     count is unchanged.
  ///   - Mixer step: `channelsIn` must match current count; count
  ///     becomes `channelsOut`.
  /// After the walk, the count must equal the playback channel count.
  private static func validatePipeline(_ config: DSPConfiguration) throws {
    var numChannels = config.devices.capture.channels

    if let pipeline = config.pipeline {
      for (i, step) in pipeline.enumerated() {
        // A bypassed step is skipped during processing and does not
        // affect channel counts.
        if step.bypassed == true { continue }

        switch step.type {
        case .filter:
          guard let names = step.names, !names.isEmpty else {
            throw ConfigError.invalidPipeline("Filter step \(i) must have 'names'")
          }
          guard step.channel != nil || step.channels != nil else {
            throw ConfigError.invalidPipeline("Filter step \(i) must have 'channel' or 'channels'")
          }
          for name in names {
            guard config.filters?[name] != nil else {
              throw ConfigError.invalidPipeline(
                "Filter '\(name)' referenced in pipeline but not defined")
            }
          }
          var channelIndices: [Int] = []
          if let ch = step.channel { channelIndices = [ch] }
          if let chs = step.channels { channelIndices = chs }
          for ch in channelIndices {
            guard ch < numChannels else {
              throw ConfigError.invalidPipeline(
                "Filter step \(i) references channel \(ch) but pipeline only has \(numChannels) channel(s) at this point"
              )
            }
          }

        case .mixer:
          guard let name = step.name else {
            throw ConfigError.invalidPipeline("Mixer step \(i) must have 'name'")
          }
          guard let mixerConfig = config.mixers?[name] else {
            throw ConfigError.invalidPipeline(
              "Mixer '\(name)' referenced in pipeline but not defined")
          }
          guard mixerConfig.channelsIn == numChannels else {
            throw ConfigError.invalidPipeline(
              "Mixer '\(name)' expects \(mixerConfig.channelsIn) input channel(s) but pipeline has \(numChannels) at this point"
            )
          }
          numChannels = mixerConfig.channelsOut
        }
      }
    }

    let playbackChannels = config.devices.playback.channels
    guard numChannels == playbackChannels else {
      throw ConfigError.invalidPipeline(
        "Pipeline outputs \(numChannels) channel(s) but playback device expects \(playbackChannels)"
      )
    }
  }
}
