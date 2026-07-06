import Foundation

/// Errors raised by `AudioResampler` implementations during construction
/// and the per-chunk `process(...)` call.
enum ResamplerError: Error, Sendable, CustomStringConvertible {
  /// `input.validFrames` did not equal the resampler's fixed `chunkSize`.
  case inputSizeMismatch(needed: Int, got: Int)
  /// Caller's output AudioChunk doesn't have enough capacity per channel.
  case outputBufferTooSmall(needed: Int, got: Int)
  /// Caller's output AudioChunk has the wrong channel count.
  case channelCountMismatch(needed: Int, got: Int)
  /// Caller passed a non-positive `channels` or `chunkSize` to init.
  case invalidParameter(message: String)
  /// The underlying system resampler refused to initialise — typically
  /// `AudioConverterNew` returning a non-zero `OSStatus`.
  case initializationFailed(message: String)

  var description: String {
    switch self {
    case .inputSizeMismatch(let needed, let got):
      return "Resampler input size mismatch: needed \(needed), got \(got)"
    case .outputBufferTooSmall(let needed, let got):
      return "Resampler output buffer too small: needed \(needed), got \(got)"
    case .channelCountMismatch(let needed, let got):
      return "Resampler channel count mismatch: needed \(needed), got \(got)"
    case .invalidParameter(let msg):
      return "Resampler invalid parameter: \(msg)"
    case .initializationFailed(let msg):
      return "Resampler initialization failed: \(msg)"
    }
  }
}
