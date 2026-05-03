// AudioHistoryBuffer — stores recent audio samples for spectrum analysis and vector scope.

import Accelerate
import Foundation

/// Maximum number of frames retained per channel. Chosen to match the Rust
/// `RING_BUFFER_CAPACITY = 262144`. At 48 kHz that's roughly 5.5 s of audio
/// — enough headroom for an FFT down to ~5 Hz.
internal let kRingBufferCapacity = 262_144

/// Owns one `SPSCAudioRingBuffer` per channel for one side (capture or
/// playback) of the engine. Resized only between engine starts, when no
/// audio thread is running. Read by consumers via `readLatest(...)`
/// (snapshot semantics — same window can be re-read for FFTs at
/// different lengths), optionally averaging across channels.
public final class AudioHistoryBuffer: @unchecked Sendable {
  public private(set) var channels: Int = 0
  private var buffers: [SPSCAudioRingBuffer] = []
  /// Preallocated scratch used by the consumer to average channels
  /// without per-call heap traffic. Sized to the ring's capacity.
  private var averagingScratch: UnsafeMutableBufferPointer<Float>?

  public init() {}

  /// Re-allocate buffers for a new channel layout. Must only be called
  /// while the engine is stopped (no producer touching the ring).
  public func reset(channels: Int) {
    self.channels = channels
    self.buffers = (0..<channels).map { _ in
      SPSCAudioRingBuffer(minimumCapacity: kRingBufferCapacity)
    }
    if averagingScratch == nil, channels > 0 {
      let scratch = UnsafeMutableBufferPointer<Float>.allocate(
        capacity: kRingBufferCapacity
      )
      scratch.initialize(repeating: 0)
      averagingScratch = scratch
    }
  }

  deinit {
    averagingScratch?.deallocate()
  }

  /// Whether any sample has been written on this side yet.
  public var hasData: Bool {
    for buffer in buffers where buffer.totalSamplesWritten > 0 { return true }
    return false
  }

  /// **Producer-only.** Forward each channel's waveform into the matching
  /// lock-free ring.
  public func append(chunk: AudioChunk) {
    guard chunk.channels == channels, channels > 0 else { return }
    let valid = chunk.validFrames
    guard valid > 0 else { return }
    for ch in 0..<channels {
      chunk.waveforms[ch].withUnsafeBufferPointer { src in
        if let srcPtr = src.baseAddress {
          buffers[ch].appendConvertingDoubleToFloat(srcPtr, count: valid)
        }
      }
    }
  }

  /// **Consumer.** Copy the most recent `count` samples for the given
  /// channel into `dest`. When `channel` is `nil` all channels are
  /// averaged into `dest`. Returns `false` if there isn't enough data
  /// yet.
  ///
  /// `dest` must have capacity for at least `count` Floats.
  public func readLatest(
    into dest: UnsafeMutablePointer<Float>,
    count: Int,
    channel: Int?
  ) throws -> Bool {
    guard channels > 0 else { throw SpectrumError.bufferEmpty }
    if let ch = channel {
      guard ch >= 0 && ch < channels else {
        throw SpectrumError.channelOutOfRange(channel: ch, available: channels)
      }
      return buffers[ch].readLatest(into: dest, count: count)
    }
    // Average across channels into `dest`. Read channel 0 directly into
    // `dest` to avoid a zeroing pass, then accumulate the rest into a
    // preallocated scratch buffer and add+divide.
    guard let scratch = averagingScratch, count <= scratch.count else {
      return false
    }
    guard buffers[0].readLatest(into: dest, count: count) else { return false }
    if channels == 1 { return true }
    for ch in 1..<channels {
      guard let scratchPtr = scratch.baseAddress else { return false }
      guard buffers[ch].readLatest(into: scratchPtr, count: count) else {
        return false
      }
      // dest += scratch (vectorised, no allocation).
      var destBuf = UnsafeMutableBufferPointer(start: dest, count: count)
      let scratchBuf = UnsafeBufferPointer(start: scratchPtr, count: count)
      vDSP.add(destBuf, scratchBuf, result: &destBuf)
    }
    let divisor = Float(channels)
    var destBuf = UnsafeMutableBufferPointer(start: dest, count: count)
    vDSP.multiply(1.0 / divisor, destBuf, result: &destBuf)
    return true
  }
}
