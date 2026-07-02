import DSPAudio
import DSPConfig
import Foundation

/// Protocol for all audio filters. Filters operate on one channel at a time.
///
/// `waveform` is a pointer into class-owned storage (`AudioBuffers`). The
/// pointer's `count` is the number of samples to process — typically the
/// owning chunk's `validFrames`, sliced down by the caller. Filters must
/// not assume the pointer covers the channel's full capacity.
public protocol Filter: AnyObject {
  /// The unique name of this filter instance.
  var name: String { get }

  /// Process a waveform buffer in-place. The buffer's `count` defines the
  /// processed range.
  func process(waveform: MutableWaveform)

  /// Update the filter parameters dynamically.
  func updateParameters(_ config: FilterConfig, sampleRate: Int)
}

/// Factory to create filter instances from configuration.
///
/// Validation runs first via `FilterConfig.validate(sampleRate:)`; the
/// switch then constructs the runtime filter for each variant. The
/// `.volume` case is reserved for the implicit master-volume filter
/// inside `Pipeline` and cannot be user-defined.
public enum FilterFactory {
  public static func create(
    name: String = "filter",
    config: FilterConfig,
    sampleRate: Int,
    chunkSize: Int
  ) throws -> Filter {
    try config.validate()
    switch config {
    case .gain(let p):
      return GainFilter(name: name, parameters: p)
    case .volume:
      throw ConfigError.invalidFilter("Volume filter cannot be created by the user")
    case .loudness(let p):
      return LoudnessFilter(name: name, parameters: p, sampleRate: sampleRate)
    case .biquad(let p):
      try p.validate(sampleRate: sampleRate)
      return try BiquadFilter(
        name: name,
        coefficients: BiquadFilter.computeCoefficients(p, sampleRate: sampleRate))
    case .conv(let p):
      return try ConvolutionFilter(
        name: name, parameters: p, chunkSize: chunkSize, sampleRate: sampleRate)
    case .delay(let p):
      return DelayFilter(name: name, parameters: p, sampleRate: sampleRate)
    case .biquadCombo(let p):
      try p.validate(sampleRate: sampleRate)
      return try BiquadComboFilter(name: name, parameters: p, sampleRate: sampleRate)
    case .diffEq(let p):
      return DiffEqFilter(name: name, parameters: p)
    case .dither(let p):
      return DitherFilter(name: name, parameters: p)
    case .limiter(let p):
      return LimiterFilter(name: name, parameters: p)
    case .lookaheadLimiter(let p):
      try p.validate(sampleRate: sampleRate)
      return LookaheadLimiterFilter(
        name: name, parameters: p, sampleRate: sampleRate, chunkSize: chunkSize)
    }
  }
}
