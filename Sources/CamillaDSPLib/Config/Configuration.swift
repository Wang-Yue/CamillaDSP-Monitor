// Configuration parsing + validation. CamillaDSP-Monitor only ever sends JSON
// over the actor's `start(configJson:)` boundary, so the loader is JSON-only;
// the upstream YAML pathway and Yams dependency have been pruned.

import Foundation
import Logging

public enum ConfigError: Error, CustomStringConvertible {
  case parseError(String)
  case validationError(String)
  case invalidFilter(String)
  case invalidMixer(String)
  case invalidPipeline(String)

  public var description: String {
    switch self {
    case .parseError(let msg): return "Parse error: \(msg)"
    case .validationError(let msg): return "Validation error: \(msg)"
    case .invalidFilter(let msg): return "Invalid filter: \(msg)"
    case .invalidMixer(let msg): return "Invalid mixer: \(msg)"
    case .invalidPipeline(let msg): return "Invalid pipeline: \(msg)"
    }
  }
}

public final class ConfigLoader {
  private static let logger = Logger(label: "camilladsp.config")

  /// Parse a CamillaDSP configuration from JSON.
  public static func parse(json: String) throws -> CamillaDSPConfig {
    guard let data = json.data(using: .utf8) else {
      throw ConfigError.parseError("JSON config is not valid UTF-8")
    }
    do {
      let config = try JSONDecoder().decode(CamillaDSPConfig.self, from: data)
      try validate(config)
      return config
    } catch let error as DecodingError {
      throw ConfigError.parseError("\(error)")
    }
  }

  /// Validate the configuration
  public static func validate(_ config: CamillaDSPConfig) throws {
    // Validate sample rate
    guard config.devices.samplerate > 0 else {
      throw ConfigError.validationError("Sample rate must be positive")
    }

    // Validate chunk size (must be power of 2 for FFT efficiency)
    guard config.devices.chunksize > 0 else {
      throw ConfigError.validationError("Chunk size must be positive")
    }

    // Validate channels
    guard config.devices.capture.channels > 0 else {
      throw ConfigError.validationError("Capture channels must be positive")
    }
    guard config.devices.playback.channels > 0 else {
      throw ConfigError.validationError("Playback channels must be positive")
    }

    // Validate individual filter configs
    let sampleRate = config.devices.samplerate
    if let filters = config.filters {
      for (name, filterConfig) in filters {
        do {
          try FilterValidator.validate(filterConfig, sampleRate: sampleRate)
        } catch {
          throw ConfigError.invalidFilter("Filter '\(name)': \(error)")
        }
      }
    }

    // Validate individual mixer configs
    if let mixers = config.mixers {
      for (name, mixerConfig) in mixers {
        do {
          try MixerValidator.validate(mixerConfig)
        } catch {
          throw ConfigError.invalidMixer("Mixer '\(name)': \(error)")
        }
      }
    }

    // Walk the pipeline tracking the channel count through each step.
    // Mirrors the logic in the Rust config::utils::validate_config pipeline walk:
    //   - Filter: all channel indices must be < current count; count unchanged
    //   - Mixer: channelsIn must match current count; count becomes channelsOut
    // After the walk, the count must equal the playback channel count.
    var numChannels = config.devices.capture.channels

    if let pipeline = config.pipeline {
      for (i, step) in pipeline.enumerated() {
        // A bypassed step is skipped during processing and does not affect channel counts.
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
          // Collect the explicit channel indices and verify they are in range.
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
        // numChannels is unchanged by a filter step.

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
          // After the mixer, the channel count changes to its output count.
          numChannels = mixerConfig.channelsOut
        }
      }
    }

    // The channel count exiting the pipeline must match what the playback device expects.
    let playbackChannels = config.devices.playback.channels
    guard numChannels == playbackChannels else {
      throw ConfigError.invalidPipeline(
        "Pipeline outputs \(numChannels) channel(s) but playback device expects \(playbackChannels)"
      )
    }

    logger.info("Configuration validated successfully")
  }

}
