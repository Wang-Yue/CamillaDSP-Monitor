// Non-interleaved float buffers, one vector per channel.

import Accelerate
import Foundation

/// A chunk of non-interleaved audio data flowing through the pipeline.
///
/// Storage is class-backed (`AudioBuffers`) so per-channel mutable pointers
/// stay stable across struct copies and the audio thread can mutate samples
/// without going through Swift's CoW uniqueness check. Two `AudioChunk`
/// values that share an `AudioBuffers` see the same samples — this is a
/// deliberate trade against the old `[[Double]]` value semantics, made to
/// remove allocations on the hot path.
public struct AudioChunk: @unchecked Sendable {
  /// Per-channel sample capacity (== `buffers.capacity`).
  public var frames: Int { buffers.capacity }
  /// Number of channels.
  public var channels: Int { buffers.channels }

  /// Number of valid frames (may be < `frames` at end-of-stream).
  public var validFrames: Int
  /// Class-owned, contiguous per-channel sample storage.
  public let buffers: AudioBuffers

  /// Create a new silent AudioChunk with freshly allocated storage.
  public init(frames: Int, channels: Int) {
    self.validFrames = frames
    self.buffers = AudioBuffers(channels: channels, capacity: frames)
  }

  /// Create an AudioChunk that adopts the given `AudioBuffers`. Zero-copy.
  public init(buffers: AudioBuffers, validFrames: Int? = nil) {
    self.validFrames = validFrames ?? buffers.capacity
    self.buffers = buffers
  }

  /// Direct mutable per-channel pointer. The pointer is stable for the
  /// lifetime of the underlying `AudioBuffers` and aliases across struct
  /// copies — no CoW.
  @inlinable
  public subscript(ch: Int) -> MutableWaveform {
    buffers[ch]
  }
}

/// A preallocated round-robin pool of unique `AudioChunk` instances.
/// Guarantees zero-allocation rotation tailored to real-time thread loops.
struct RoundRobinChunkPool {
  @usableFromInline
  internal let pool: [AudioChunk]
  @usableFromInline
  internal var currentIndex: Int = 0

  init(capacity: Int, frames: Int, channels: Int) {
    precondition(capacity > 0, "Pool capacity must be positive")
    self.pool = (0..<capacity).map { _ in AudioChunk(frames: frames, channels: channels) }
  }

  /// Retrieves the next available unique chunk buffer from the pool.
  @inlinable
  mutating func next() -> AudioChunk {
    let chunk = pool[currentIndex]
    currentIndex = (currentIndex + 1) % pool.count
    return chunk
  }
}

extension AudioChunk {
  /// Sums multiple channels of a chunk into a single destination buffer.
  @inlinable
  public func sumChannels(
    _ channels: [Int],
    into dest: UnsafeMutablePointer<Double>,
    count: Int
  ) {
    guard !channels.isEmpty else { return }
    let ch0 = channels[0]
    guard let src0Base = self[ch0].baseAddress else { return }
    dest.initialize(from: src0Base, count: count)
    for chIdx in 1..<channels.count {
      let ch = channels[chIdx]
      guard let srcBase = self[ch].baseAddress else { continue }
      vDSP_vaddD(dest, 1, srcBase, 1, dest, 1, vDSP_Length(count))
    }
  }

  /// Applies sample-by-sample linear gains to multiple channels.
  @inlinable
  public func applyGain(
    to channels: [Int],
    from gainMultipliers: UnsafePointer<Double>,
    count: Int
  ) {
    for ch in channels {
      guard let waveBase = self[ch].baseAddress else { continue }
      vDSP_vmulD(waveBase, 1, gainMultipliers, 1, waveBase, 1, vDSP_Length(count))
    }
  }
}
