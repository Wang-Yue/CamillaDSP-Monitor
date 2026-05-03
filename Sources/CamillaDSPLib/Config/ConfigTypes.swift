// CamillaDSP configuration types — maps to the JSON sent by CamillaDSP-Monitor's
// `DSPEngineController.buildConfigDict()`. Only the subset of fields the
// Monitor actually emits (and CoreAudio actually needs) is modelled here.

import Foundation

// MARK: - Top-level Configuration

public struct CamillaDSPConfig: Codable {
  public var title: String?
  public var description: String?
  public var devices: DevicesConfig
  public var filters: [String: FilterConfig]?
  public var mixers: [String: MixerConfig]?
  public var pipeline: [PipelineStep]?

  public init(devices: DevicesConfig) { self.devices = devices }
}

// MARK: - Devices

public struct DevicesConfig: Codable, Equatable {
  public var samplerate: Int
  public var chunksize: Int
  public var enableRateAdjust: Bool?
  public var targetLevel: Int?
  public var adjustPeriod: Double?
  public var volumeRampTime: Double?
  public var volumeLimit: Double?
  public var multithreaded: Bool?
  public var workerThreads: Int?
  public var resampler: ResamplerConfig?
  public var capture: CaptureDeviceConfig
  public var playback: PlaybackDeviceConfig
  /// Capture sample rate when different from playback (requires resampler)
  public var captureSamplerate: Int?
  /// Silence detection threshold (dB). 0 = disabled.
  public var silenceThreshold: Double?
  /// Silence detection timeout (seconds). 0 = disabled.
  public var silenceTimeout: Double?
  /// Stop processing on sample rate change (requires restart)
  public var stopOnRateChange: Bool?
  /// Max number of chunks in the playback queue
  public var queuelimit: Int?
  /// Interval in seconds for rate measurement
  public var rateMeasureInterval: Double?

  enum CodingKeys: String, CodingKey {
    case samplerate, chunksize, resampler, capture, playback, queuelimit, multithreaded
    case enableRateAdjust = "enable_rate_adjust"
    case targetLevel = "target_level"
    case adjustPeriod = "adjust_period"
    case volumeRampTime = "volume_ramp_time"
    case volumeLimit = "volume_limit"
    case workerThreads = "worker_threads"
    case captureSamplerate = "capture_samplerate"
    case silenceThreshold = "silence_threshold"
    case silenceTimeout = "silence_timeout"
    case stopOnRateChange = "stop_on_rate_change"
    case rateMeasureInterval = "rate_measure_interval"
  }

  public init(
    samplerate: Int, chunksize: Int, capture: CaptureDeviceConfig, playback: PlaybackDeviceConfig
  ) {
    self.samplerate = samplerate
    self.chunksize = chunksize
    self.capture = capture
    self.playback = playback
  }
}

public struct CaptureDeviceConfig: Codable, Equatable {
  public var type: AudioBackendType
  public var channels: Int
  public var device: String?
  public var format: SampleFormat?
  public var labels: [String?]?

  enum CodingKeys: String, CodingKey {
    case type, channels, device, format, labels
  }
  public init(
    type: AudioBackendType, channels: Int, device: String? = nil, format: SampleFormat? = nil,
    labels: [String?]? = nil
  ) {
    self.type = type
    self.channels = channels
    self.device = device
    self.format = format
    self.labels = labels
  }
}

public struct PlaybackDeviceConfig: Codable, Equatable {
  public var type: AudioBackendType
  public var channels: Int
  public var device: String?
  public var format: SampleFormat?
  public var exclusive: Bool?
  public var labels: [String?]?

  enum CodingKeys: String, CodingKey {
    case type, channels, device, format, exclusive, labels
  }
  public init(
    type: AudioBackendType, channels: Int, device: String? = nil, format: SampleFormat? = nil,
    exclusive: Bool? = nil, labels: [String?]? = nil
  ) {
    self.type = type
    self.channels = channels
    self.device = device
    self.format = format
    self.exclusive = exclusive
    self.labels = labels
  }
}

/// Audio I/O backend. CamillaDSP-Monitor only ever uses CoreAudio.
public enum AudioBackendType: String, Codable, Equatable {
  case coreAudio = "CoreAudio"
}

// MARK: - Resampler

public struct ResamplerConfig: Codable, Equatable {
  public var type: ResamplerType
  public var profile: ResamplerProfile?
  public var sincLen: Int?
  public var oversamplingFactor: Int?

  enum CodingKeys: String, CodingKey {
    case type, profile
    case sincLen = "sinc_len"
    case oversamplingFactor = "oversampling_factor"
  }

  public init(type: ResamplerType, profile: ResamplerProfile? = nil) {
    self.type = type
    self.profile = profile
  }
}

public enum ResamplerType: String, Codable, Equatable {
  case asyncSinc = "AsyncSinc"
  case asyncPoly = "AsyncPoly"
  case synchronous = "Synchronous"
}

public enum ResamplerProfile: String, Codable, Equatable {
  case veryFast = "VeryFast"
  case fast = "Fast"
  case balanced = "Balanced"
  case accurate = "Accurate"
}

// MARK: - Filters

public struct FilterConfig: Codable {
  public var type: FilterType
  public var parameters: FilterParameters

  public init(type: FilterType, parameters: FilterParameters) {
    self.type = type
    self.parameters = parameters
  }
}

/// CamillaDSP-Monitor only emits Gain, Volume, Loudness, and Biquad filters.
/// Anything else would never reach the engine, so we don't model it.
public enum FilterType: String, Codable {
  case gain = "Gain"
  case volume = "Volume"
  case loudness = "Loudness"
  case biquad = "Biquad"
}

public struct FilterParameters: Codable {
  public init() {}

  // Gain
  public var gain: Double?
  public var scale: GainScale?
  public var inverted: Bool?
  public var mute: Bool?

  // Volume
  public var rampTime: Double?
  public var limit: Double?

  // Loudness
  public var referenceLevel: Double?
  public var highBoost: Double?
  public var lowBoost: Double?
  public var attenuateMid: Bool?

  // Biquad - the JSON `type` key inside `parameters` selects the subtype.
  public var subtype: String?
  public var biquadType: BiquadType? { subtype.flatMap { BiquadType(rawValue: $0) } }
  public var freq: Double?
  public var q: Double?
  public var slope: Double?
  public var bandwidth: Double?
  public var a1: Double?
  public var a2: Double?
  public var b0: Double?
  public var b1: Double?
  public var b2: Double?
  public var freqNotch: Double?
  public var freqPole: Double?
  public var normalizeAtDc: Bool?
  public var freqAct: Double?
  public var qAct: Double?
  public var freqTarget: Double?
  public var qTarget: Double?

  enum CodingKeys: String, CodingKey {
    case gain, scale, inverted, mute
    case rampTime = "ramp_time"
    case limit
    case referenceLevel = "reference_level"
    case highBoost = "high_boost"
    case lowBoost = "low_boost"
    case attenuateMid = "attenuate_mid"
    case subtype = "type"
    case freq, q, slope, bandwidth
    case a1, a2, b0, b1, b2
    case freqNotch = "freq_notch"
    case freqPole = "freq_pole"
    case normalizeAtDc = "normalize_at_dc"
    case freqAct = "freq_act"
    case qAct = "q_act"
    case freqTarget = "freq_target"
    case qTarget = "q_target"
  }
}

public enum GainScale: String, Codable {
  case dB
  case linear
}

public enum BiquadType: String, Codable {
  case free = "Free"
  case highpass = "Highpass"
  case lowpass = "Lowpass"
  case highpassFO = "HighpassFO"
  case lowpassFO = "LowpassFO"
  case highshelf = "Highshelf"
  case lowshelf = "Lowshelf"
  case highshelfFO = "HighshelfFO"
  case lowshelfFO = "LowshelfFO"
  case peaking = "Peaking"
  case notch = "Notch"
  case generalNotch = "GeneralNotch"
  case bandpass = "Bandpass"
  case allpass = "Allpass"
  case allpassFO = "AllpassFO"
  case linkwitzTransform = "LinkwitzTransform"
}

// MARK: - Mixers

/// Helper for decoding the Rust nested format: `channels: { in: N, out: N }`
private struct MixerChannelsNested: Codable {
  var `in`: Int
  var out: Int
}

public struct MixerConfig: Codable {
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
}

public struct MixerMapping: Codable {
  public var dest: Int
  public var sources: [MixerSource]
  public var mute: Bool?

  public init(dest: Int, sources: [MixerSource], mute: Bool? = nil) {
    self.dest = dest
    self.sources = sources
    self.mute = mute
  }
}

public struct MixerSource: Codable {
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

// MARK: - Pipeline

public struct PipelineStep: Codable {
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

public enum PipelineStepType: String, Codable {
  case filter = "Filter"
  case mixer = "Mixer"
}
