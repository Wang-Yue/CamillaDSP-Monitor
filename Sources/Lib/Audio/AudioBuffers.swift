// Class-backed, contiguous per-channel audio storage.
//
// Replaces the old `[[PrcFmt]]` ("array of arrays") chunk storage. The 2-D
// nested-array layout had two costs the audio thread couldn't afford:
//
//   1. `array[ch].withUnsafeMutableBufferPointer { ... }` triggers Swift's
//      uniqueness check on the inner buffer; whenever any external reference
//      kept the inner array's storage shared (closures, queues, captures),
//      the next mutable access malloc'd a fresh copy.
//   2. The outer array's element copies bumped per-channel buffer refcounts
//      on every value-copy of an `AudioChunk`.
//
// `AudioBuffers` allocates one contiguous block of `channels * capacity`
// `PrcFmt` values up front and exposes per-channel `UnsafeMutableBufferPointer`
// views that are stable for the buffer's lifetime. The hot path uses the
// pointers directly — no `withUnsafe*` round trips, no uniqueness checks.
//
// Thread-safety: `AudioBuffers` itself does no synchronisation. The pipeline
// already enforces single-writer discipline (the audio thread owns each
// buffer while it processes a chunk), so `@unchecked Sendable` is honest
// here — the type is no less safe than the `[[PrcFmt]]` it replaces.

import Foundation

/// Contiguous, per-channel audio storage backed by a single heap allocation.
public final class AudioBuffers: @unchecked Sendable {
  /// Number of channels.
  public let channels: Int
  /// Per-channel capacity in `PrcFmt` samples.
  public let capacity: Int

  /// One contiguous `channels * capacity` block. Channel `ch` lives at
  /// `[ch * capacity ..< (ch + 1) * capacity]`.
  private let storage: UnsafeMutablePointer<PrcFmt>

  /// Pre-built per-channel views — sized to `capacity`, pointing into
  /// `storage`. Built once at init and never resized; the pointers stay
  /// valid for the entire lifetime of this `AudioBuffers`.
  @usableFromInline
  internal let channelBuffers: [MutableWaveform]

  /// Allocate a fresh buffer pool, zero-initialised.
  public init(channels: Int, capacity: Int) {
    precondition(channels > 0, "channels must be positive")
    precondition(capacity > 0, "capacity must be positive")
    self.channels = channels
    self.capacity = capacity

    let total = channels * capacity
    let storage = UnsafeMutablePointer<PrcFmt>.allocate(capacity: total)
    storage.initialize(repeating: 0, count: total)
    self.storage = storage

    var bufs: [MutableWaveform] = []
    bufs.reserveCapacity(channels)
    for ch in 0..<channels {
      bufs.append(
        MutableWaveform(start: storage + ch * capacity, count: capacity))
    }
    self.channelBuffers = bufs
  }

  deinit {
    storage.deinitialize(count: channels * capacity)
    storage.deallocate()
  }

  /// Mutable per-channel pointer. The pointer is stable for the lifetime
  /// of the `AudioBuffers`; callers may cache it.
  @inlinable
  public subscript(ch: Int) -> MutableWaveform {
    channelBuffers[ch]
  }
}
