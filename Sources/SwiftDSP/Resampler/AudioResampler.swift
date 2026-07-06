// Resampler protocol + shared types.
// Four resampler implementations conform to `AudioResampler`:
//   * `SynchronousResampler` — FFT-based fixed-ratio.
//   * `AsyncSincResampler`   — Asynchronous windowed-sinc resampler.
//   * `AsyncPolyResampler`   — Asynchronous polynomial resampler.
//   * `AppleResampler`       — Core Audio AudioConverter wrapper.

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
  case .asyncSinc:
    if let sincLen = config.sincLen,
      let oversamplingFactor = config.oversamplingFactor,
      let windowStr = config.window,
      let window = WindowFunction(rawValue: windowStr),
      let interpStr = config.interpolation,
      let interpolation = SincInterpolationType(rawValue: interpStr)
    {
      return AsyncSincResampler(
        channels: channels, inputRate: inputRate, outputRate: outputRate,
        sincLen: sincLen, oversamplingFactor: oversamplingFactor,
        interpolation: interpolation, window: window, fCutoff: config.fCutoff,
        chunkSize: chunkSize)
    } else {
      let profile = config.profile.flatMap { ResamplerProfile(rawValue: $0) } ?? .balanced
      return AsyncSincResampler(
        channels: channels, inputRate: inputRate, outputRate: outputRate,
        profile: profile, chunkSize: chunkSize)
    }
  case .asyncPoly:
    let interp = config.interpolation.flatMap { PolyInterpolation(rawValue: $0) } ?? .cubic
    return AsyncPolyResampler(
      channels: channels, inputRate: inputRate, outputRate: outputRate,
      interpolation: interp, chunkSize: chunkSize)
  case .apple:
    return try AppleResampler(
      channels: channels, inputRate: inputRate, outputRate: outputRate,
      quality: config.appleQuality ?? .max,
      complexity: config.appleComplexity ?? .normal,
      chunkSize: chunkSize)
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

  /// Current effective ratio (`base * relative`).
  var ratio: Double { get }

  /// Apply a multiplicative correction on top of the base ratio.
  /// `SynchronousResampler` ignores this (its ratio is fixed by
  /// construction).
  func setRelativeRatio(_ multiplier: Double)
}

/// Polynomial degree exposed by `AsyncPolyResampler`.
enum PolyInterpolation: String, Codable {
  case linear = "Linear"
  case cubic = "Cubic"
  case quintic = "Quintic"
  case septic = "Septic"

  /// Number of input samples the polynomial is fitted across.
  /// Number of interpolation points.
  var nbrPoints: Int {
    switch self {
    case .linear: return 2
    case .cubic: return 4
    case .quintic: return 6
    case .septic: return 8
    }
  }
}

enum SincInterpolationType: String, CaseIterable {
  case linear = "Linear"
  case quadratic = "Quadratic"
  case cubic = "Cubic"
}
