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
  /// Process a waveform buffer in-place. The buffer's `count` defines the
  /// processed range.
  func process(waveform: MutableWaveform)
}

/// Factory to create filter instances from configuration.
///
/// Validation runs first via `FilterConfig.validate(sampleRate:)`; the
/// switch then constructs the runtime filter for each variant. The
/// `.volume` case is reserved for the implicit master-volume filter
/// inside `Pipeline` and cannot be user-defined.
public enum FilterFactory {
  public static func create(
    config: FilterConfig,
    sampleRate: Int
  ) throws -> Filter {
    try config.validate()
    switch config {
    case .gain(let p):
      return GainFilter(parameters: p)
    case .volume:
      throw ConfigError.invalidFilter("Volume filter cannot be created by the user")
    case .loudness(let p):
      return LoudnessFilter(parameters: p, sampleRate: sampleRate)
    case .biquad(let p):
      try p.validate(sampleRate: sampleRate)
      return try BiquadFilter(
        coefficients: BiquadFilter.computeCoefficients(p, sampleRate: sampleRate))

    }
  }
}
