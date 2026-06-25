// Resampler protocol + shared types.
// The resampler implementation conforms to `AudioResampler`:
//   * `SynchronousResampler` — FFT-based fixed-ratio.

import DSPAudio
import DSPConfig
import Foundation

public func createResampler(
  config: ResamplerConfig, inputRate: Int, outputRate: Int, channels: Int, chunkSize: Int
) throws -> AudioResampler {
  switch config.type {
  case .synchronous:
    return SynchronousResampler(
      channels: channels, inputRate: inputRate, outputRate: outputRate,
      chunkSize: chunkSize)
  case .asyncSinc, .asyncPoly:
    throw ResamplerError.invalidParameter(
      message: "Resampler type \(config.type.rawValue) is not supported by the native Swift engine"
    )
  }
}

/// Resampler protocol.
///
/// Each resampler is initialised with a *base* ratio of `outputRate / inputRate`,
/// a *fixed* `chunkSize` (the number of input frames every `process` call must
/// receive), and a *relative* multiplier (`1.0` by default) that the rate-adjust
/// loop nudges to track clock drift. The effective ratio per chunk is
/// `base * relative`.
///
/// Because `chunkSize` is fixed at construction, the implementations
/// pre-allocate every internal buffer at init and never allocate on the hot
/// path. The caller must supply pre-allocated output buffers via
/// `process(input:into:)`.
public protocol AudioResampler: AnyObject {
  /// Input frames the resampler expects on every `process` call.
  var chunkSize: Int { get }

  /// Number of channels processed per call.
  var channels: Int { get }

  /// Zero-allocation API. The caller pre-allocates `output` with
  /// `output.channels == channels` and `output.frames >= maxOutputFrames`.
  /// The resampler writes the produced samples in place and updates
  /// `output.validFrames`.
  ///
  /// Throws `ResamplerError.inputSizeMismatch` if `input.validFrames` does
  /// not equal `chunkSize`, `outputBufferTooSmall` if the per-channel buffers
  /// can't fit the output, or `channelCountMismatch` if the channel layout
  /// doesn't match.
  func process(input: AudioChunk, into output: inout AudioChunk) throws

  /// Worst-case output frames across the resampler's allowed ratio range —
  /// use this to size the output `AudioChunk` once at startup.
  var maxOutputFrames: Int { get }

  /// Apply a multiplicative correction on top of the base ratio.
  /// `SynchronousResampler` ignores this (its ratio is fixed by
  /// construction).
  func setRelativeRatio(_ multiplier: Double)
}
