// Top-level configuration data structures and validation logic. The JSON loader
// lives in `ConfigLoader.swift`.
//
// This file owns:
//   1. Top-level configuration models (DSPConfiguration and PipelineStep).
//   2. Cross-component validation logic, including schema checks and the
//      pipeline walk that tracks channel layouts.

import Foundation

/// Top-level configuration consumed by the DSP engine.
public struct DSPConfiguration: Codable, Sendable, Equatable {

  public var devices: DevicesConfig
  public var filters: [String: FilterConfig]?
  public var mixers: [String: MixerConfig]?
  public var processors: [String: ProcessorConfig]?
  public var pipeline: [PipelineStep]?

  public init(devices: DevicesConfig) { self.devices = devices }
}

/// One step in the user-defined processing pipeline. Either a named
/// filter chain applied to one or more channels, or a mixer that
/// changes the channel layout.
public struct PipelineStep: Codable, Sendable, Equatable {
  public var type: PipelineStepType
  public var channel: Int?
  public var channels: [Int]?
  public var name: String?
  public var names: [String]?
  public var bypassed: Bool?

  public init(
    type: PipelineStepType, channel: Int? = nil, channels: [Int]? = nil,
    name: String? = nil, names: [String]? = nil, bypassed: Bool? = nil
  ) {
    self.type = type
    self.channel = channel
    self.channels = channels
    self.name = name
    self.names = names
    self.bypassed = bypassed
  }
}

public enum PipelineStepType: String, Codable, Sendable {
  case filter = "Filter"
  case mixer = "Mixer"
  case processor = "Processor"
}

extension DSPConfiguration {
  public func validate() throws {
    try validateTopLevelFields()

    if let filters = filters {
      for (name, filterConfig) in filters {
        do {
          try filterConfig.validate(sampleRate: devices.samplerate)
        } catch {
          throw ConfigError.invalidFilter("Filter '\(name)': \(error)")
        }
      }
    }

    if let mixers = mixers {
      for (name, mixerConfig) in mixers {
        do {
          try mixerConfig.validate()
        } catch {
          throw ConfigError.invalidMixer("Mixer '\(name)': \(error)")
        }
      }
    }

    if let processors = processors {
      for (name, processorConfig) in processors {
        do {
          try processorConfig.validate()
        } catch {
          throw ConfigError.invalidFilter("Processor '\(name)': \(error)")
        }
      }
    }

    try validatePipeline()
  }

  private func validateTopLevelFields() throws {
    guard devices.samplerate > 0 else {
      throw ConfigError.validationError("Sample rate must be positive")
    }
    guard devices.chunksize > 0 else {
      throw ConfigError.validationError("Chunk size must be positive")
    }
    if let captureChannels = devices.capture.channels {
      guard captureChannels > 0 else {
        throw ConfigError.validationError("Capture channels must be positive")
      }
    } else {
      guard case .wavFile = devices.capture else {
        throw ConfigError.validationError("Capture channels is required")
      }
    }
    guard devices.playback.channels > 0 else {
      throw ConfigError.validationError("Playback channels must be positive")
    }

    if let timeout = devices.silenceTimeout {
      guard timeout >= 0.0 else {
        throw ConfigError.validationError("silence_timeout cannot be negative")
      }
    }
    if let threshold = devices.silenceThreshold {
      guard threshold <= 0.0 else {
        throw ConfigError.validationError("silence_threshold must be less than or equal to 0")
      }
    }
    if let limit = devices.volumeLimit {
      guard limit >= -150.0 && limit <= 50.0 else {
        throw ConfigError.validationError("Volume limit must be between -150 and +50 dB")
      }
    }
    if let targetLevel = devices.targetLevel {
      let qlimit = devices.queuelimit ?? 4
      let targetLimit = (2 + qlimit) * devices.chunksize
      guard targetLevel <= targetLimit else {
        throw ConfigError.validationError("target_level cannot be larger than \(targetLimit)")
      }
    }
  }

  private func validatePipeline() throws {
    guard var numChannels = devices.capture.channels else {
      // Bypassed if capture channels are dynamic (e.g. WavFile)
      return
    }

    if let pipeline = pipeline {
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
            guard filters?[name] != nil else {
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
          guard let mixerConfig = mixers?[name] else {
            throw ConfigError.invalidPipeline(
              "Mixer '\(name)' referenced in pipeline but not defined")
          }
          guard mixerConfig.channelsIn == numChannels else {
            throw ConfigError.invalidPipeline(
              "Mixer '\(name)' expects \(mixerConfig.channelsIn) input channel(s) but pipeline has \(numChannels) at this point"
            )
          }
          numChannels = mixerConfig.channelsOut

        case .processor:
          guard let name = step.name else {
            throw ConfigError.invalidPipeline("Processor step \(i) must have 'name'")
          }
          guard let processorConfig = processors?[name] else {
            throw ConfigError.invalidPipeline(
              "Processor '\(name)' referenced in pipeline but not defined")
          }
          let expectedChannels: Int
          switch processorConfig {
          case .compressor(let p): expectedChannels = p.channels
          case .noiseGate(let p): expectedChannels = p.channels
          case .race(let p): expectedChannels = p.channels
          }
          guard expectedChannels == numChannels else {
            throw ConfigError.invalidPipeline(
              "Processor '\(name)' expects \(expectedChannels) channel(s) but pipeline has \(numChannels) at this point"
            )
          }
        }
      }
    }

    let playbackChannels = devices.playback.channels
    guard numChannels == playbackChannels else {
      throw ConfigError.invalidPipeline(
        "Pipeline outputs \(numChannels) channel(s) but playback device expects \(playbackChannels)"
      )
    }
  }
}
