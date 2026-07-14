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


