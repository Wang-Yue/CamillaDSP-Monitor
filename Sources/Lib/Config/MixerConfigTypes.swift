// CamillaDSP-Swift: Standalone mixer configuration types.

import Foundation

private struct MixerChannelsNested: Codable, Equatable, Sendable {
  var `in`: Int
  var out: Int
}

public struct MixerConfig: Codable, Equatable, Sendable {
  public var channelsIn: Int
  public var channelsOut: Int
  public var mapping: [MixerMapping]
  public var description: String?
  public var labels: [String?]?

  public init(channelsIn: Int, channelsOut: Int, mapping: [MixerMapping]) {
    self.channelsIn = channelsIn
    self.channelsOut = channelsOut
    self.mapping = mapping
  }

  // Support both Rust nested format `channels: { in: N, out: N }` and
  // flat format `channels_in: N, channels_out: N`
  private enum CodingKeys: String, CodingKey {
    case channels  // Rust nested format
    case channelsIn = "channels_in"  // flat format
    case channelsOut = "channels_out"  // flat format
    case mapping
    case description
    case labels
  }

  public init(from decoder: Decoder) throws {
    let container = try decoder.container(keyedBy: CodingKeys.self)
    mapping = try container.decode([MixerMapping].self, forKey: .mapping)
    description = try container.decodeIfPresent(String.self, forKey: .description)
    labels = try container.decodeIfPresent([String?].self, forKey: .labels)

    // Try nested format first: channels: { in: N, out: N }
    if let nested = try? container.decode(MixerChannelsNested.self, forKey: .channels) {
      channelsIn = nested.in
      channelsOut = nested.out
    } else {
      // Fall back to flat format: channels_in / channels_out
      channelsIn = try container.decode(Int.self, forKey: .channelsIn)
      channelsOut = try container.decode(Int.self, forKey: .channelsOut)
    }
  }

  public func encode(to encoder: Encoder) throws {
    var container = encoder.container(keyedBy: CodingKeys.self)
    // Encode in the Rust-compatible nested format
    let nested = MixerChannelsNested(in: channelsIn, out: channelsOut)
    try container.encode(nested, forKey: .channels)
    try container.encode(mapping, forKey: .mapping)
    try container.encodeIfPresent(description, forKey: .description)
    try container.encodeIfPresent(labels, forKey: .labels)
  }

  /// Validate the mapping is internally consistent: every dest is in
  /// range, no dest appears twice, and within a single dest no source
  /// channel appears twice.
  public func validate() throws {
    var seenDests = Set<Int>()
    for map in mapping {
      guard map.dest < channelsOut else {
        throw ConfigError.invalidMixer(
          "mixer dest \(map.dest) >= channels_out \(channelsOut)")
      }
      guard !seenDests.contains(map.dest) else {
        throw ConfigError.invalidMixer("mixer dest \(map.dest) mapped more than once")
      }
      seenDests.insert(map.dest)

      var seenSources = Set<Int>()
      for src in map.sources {
        guard src.channel < channelsIn else {
          throw ConfigError.invalidMixer(
            "mixer source channel \(src.channel) >= channels_in \(channelsIn)")
        }
        guard !seenSources.contains(src.channel) else {
          throw ConfigError.invalidMixer(
            "mixer source channel \(src.channel) listed more than once for dest \(map.dest)")
        }
        seenSources.insert(src.channel)
      }
    }
  }
}

public struct MixerMapping: Codable, Equatable, Sendable {
  public var dest: Int
  public var sources: [MixerSource]
  public var mute: Bool?

  public init(dest: Int, sources: [MixerSource], mute: Bool? = nil) {
    self.dest = dest
    self.sources = sources
    self.mute = mute
  }
}

public struct MixerSource: Codable, Equatable, Sendable {
  public var channel: Int
  /// Gain value. Optional in Rust YAML (defaults to 0.0 dB when omitted).
  public var gain: Double?
  public var inverted: Bool?
  public var mute: Bool?
  public var scale: GainScale?

  /// Convenience accessor matching Rust default: 0.0 when gain is nil
  public var gainValue: Double { gain ?? 0.0 }

  public init(
    channel: Int, gain: Double? = nil, inverted: Bool? = nil, mute: Bool? = nil,
    scale: GainScale? = nil
  ) {
    self.channel = channel
    self.gain = gain
    self.inverted = inverted
    self.mute = mute
    self.scale = scale
  }
}
