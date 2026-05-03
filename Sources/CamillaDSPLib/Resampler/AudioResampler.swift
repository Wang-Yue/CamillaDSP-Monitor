// CamillaDSP-Swift: Resampler protocol + shared types.
// All three resampler implementations (AsyncSinc, AsyncPoly, Synchronous)
// conform to `AudioResampler`.

import Foundation

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
  /// `output.waveforms.count == channels` and each
  /// `output.waveforms[ch].count >= maxOutputFrames`. The resampler writes
  /// the produced samples in place and updates `output.validFrames`.
  ///
  /// Throws `ResamplerError.inputSizeMismatch` if `input.validFrames` does
  /// not equal `chunkSize`, `outputBufferTooSmall` if the per-channel buffers
  /// can't fit the output, or `channelCountMismatch` if the channel layout
  /// doesn't match.
  func process(input: AudioChunk, into output: inout AudioChunk) throws

  /// Number of output frames the next `process` call will produce given the
  /// current `lastIndex` / ratio state. May vary chunk-to-chunk for the
  /// async resamplers; constant for `Synchronous`.
  var nextOutputFrames: Int { get }

  /// Worst-case output frames across the resampler's allowed ratio range —
  /// use this to size the output `AudioChunk` once at startup.
  var maxOutputFrames: Int { get }

  /// Current effective ratio (`base * relative`).
  var ratio: Double { get }

  /// Apply a multiplicative correction on top of the base ratio.
  /// Mirrors rubato's `set_resample_ratio_relative`. Synchronous resamplers
  /// ignore this (their ratio is fixed by construction).
  func setRelativeRatio(_ multiplier: Double)
}

public enum ResamplerError: Error {
  /// `input.validFrames` did not equal the resampler's fixed `chunkSize`.
  case inputSizeMismatch(needed: Int, got: Int)
  /// Caller's output AudioChunk doesn't have enough capacity per channel.
  case outputBufferTooSmall(needed: Int, got: Int)
  /// Caller's output AudioChunk has the wrong channel count.
  case channelCountMismatch(needed: Int, got: Int)
}

/// Polynomial degree exposed by `AsyncPolyResampler`. Mirrors rubato's
/// `PolynomialDegree`.
public enum PolyInterpolation: String, Codable {
  case linear = "Linear"
  case cubic = "Cubic"
  case quintic = "Quintic"
  case septic = "Septic"

  /// Number of input samples the polynomial is fitted across.
  /// Matches rubato's `nbr_points()`.
  var nbrPoints: Int {
    switch self {
    case .linear: return 2
    case .cubic: return 4
    case .quintic: return 6
    case .septic: return 8
    }
  }
}

/// Sub-filter interpolation method used by `AsyncSincResampler`.
public enum SincInterpolationType {
  case linear
  case quadratic
  case cubic
}

// SIMD2<Double> load/store helpers. These pin Swift's compiler into
// emitting NEON 128-bit `ldur q` / `stur q` plus `fmul.2d`, `fmla.2d`,
// `fadd.2d`, etc. — the natural `SIMD2<Double>(arr[k], arr[k+1])` and
// `arr[i] = v.x; arr[i+1] = v.y` forms scalarize through `d` registers
// (verified via `otool -tvV`: 0 vector ops vs 1006 scalar). Routing
// through `loadUnaligned` / `storeBytes` keeps both lanes resident in
// `q` registers across the full butterfly.
@inline(__always)
func ldSIMD2(_ p: UnsafePointer<Double>, _ idx: Int) -> SIMD2<Double> {
  return UnsafeRawPointer(p + idx).loadUnaligned(as: SIMD2<Double>.self)
}

@inline(__always)
func stSIMD2(_ p: UnsafeMutablePointer<Double>, _ idx: Int, _ v: SIMD2<Double>) {
  UnsafeMutableRawPointer(p + idx).storeBytes(of: v, as: SIMD2<Double>.self)
}
