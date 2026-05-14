// Single-producer / single-consumer lock-free primitives used by the
// audio thread. Two shapes:
//
//   * `SPSCAudioRingBuffer` — power-of-two ring of `Float` samples with
//     two access patterns:
//       - **Consume style** (`write` + `consume`): the consumer
//         advances a read cursor; each sample is delivered exactly
//         once. Used by the CoreAudio capture and playback paths.
//       - **Snapshot style** (`appendConvertingDoubleToFloat` +
//         `readLatest`): the consumer takes the most-recent N
//         samples without advancing any cursor; the same window
//         can be re-read for FFTs at different lengths. Used by
//         `SpectrumAnalyzer`.
//     The two patterns share the same producer index, so a single
//     ring can serve either role — they don't mix on a given ring,
//     but the same primitive covers both.
//
//   * `SPSCQueue<T>` — generic SPSC FIFO queue. Used to pass
//     `AudioChunk` values between the capture, processing, and
//     playback threads inside `DSPEngineCore` without taking an
//     `NSLock`.
//
//   * `AtomicDouble` — a wait-free `Double` atom built on
//     `Atomic<UInt64>` round-tripped through the IEEE-754 bit pattern.
//     Used by the rate-adjust loop to publish the resampler ratio
//     from the playback thread to the processing thread.
//
// Real-time discipline
// --------------------
// All hot-path methods are wait-free, allocation-free, and free of
// Swift-runtime calls that could block. The producer always succeeds
// — if the consumer is so far behind that the buffer is full, the
// oldest unread data is silently overwritten (matching the original
// lock-based design's drop-on-overflow behaviour).

import Accelerate
import Synchronization

// MARK: - SPSC ring buffer

/// Lock-free SPSC ring buffer of `Float` samples. Power-of-two
/// capacity so wrap-around is a single bitmask. Producer publishes
/// new samples with a `release-store` on `writeIndex`; consumers
/// observe with an `acquire-load`, establishing happens-before
/// without locks.
///
/// Two consumer styles, both supported on the same instance — but
/// don't mix them on a single ring:
///
///   - **Consume:** call `consume(into:count:)` to drain samples.
///     Each sample is delivered to exactly one consumer call.
///     Used by `CoreAudioCapture` / `CoreAudioPlayback`.
///   - **Snapshot:** call `readLatest(into:count:)` to copy the
///     most-recent `count` samples without advancing any cursor.
///     The same samples can be re-read across calls. Used by
///     `SpectrumAnalyzer` to feed FFTs at different lengths.
public final class SPSCAudioRingBuffer: @unchecked Sendable {

  /// Capacity in samples (always a power of two).
  public let capacity: Int

  private let mask: Int
  private let storage: UnsafeMutableBufferPointer<Float>
  private let base: UnsafeMutablePointer<Float>
  /// Monotonically increasing count of samples written since
  /// allocation. The release-store synchronises with consumers'
  /// acquire-loads, so any reader that observes the new count is
  /// guaranteed to see the corresponding sample writes.
  private let writeIndex = Atomic<UInt64>(0)
  /// Monotonic samples drained by the consumer in `consume(...)`.
  /// Only used by the consume-style API; snapshot readers ignore
  /// this entirely.
  private let readIndex = Atomic<UInt64>(0)

  public init(minimumCapacity: Int) {
    let cap = SPSCAudioRingBuffer.roundUpToPowerOfTwo(Swift.max(2, minimumCapacity))
    self.capacity = cap
    self.mask = cap - 1
    let buffer = UnsafeMutableBufferPointer<Float>.allocate(capacity: cap)
    buffer.initialize(repeating: 0)
    self.storage = buffer
    guard let basePtr = buffer.baseAddress else {
      fatalError("Failed to allocate storage for SPSCAudioRingBuffer")
    }
    self.base = basePtr
  }

  deinit {
    storage.deallocate()
  }

  /// Total samples written since allocation. Observed with
  /// `.relaxed`; callers that need happens-before with the
  /// payload should use `consume` or `readLatest` instead.
  public var totalSamplesWritten: UInt64 {
    writeIndex.load(ordering: .relaxed)
  }

  /// Number of samples currently waiting to be consumed (for
  /// consume-style use). Always non-negative.
  public var availableToRead: Int {
    let w = writeIndex.load(ordering: .acquiring)
    let r = readIndex.load(ordering: .relaxed)
    return Int(w &- r)
  }

  // MARK: Producer

  /// **Producer-only.** Write `count` `Float` samples from
  /// `source` into the ring. `stride` lets the producer pull a
  /// single channel out of an interleaved buffer (`stride =
  /// channels`); pass `1` for non-interleaved input. Always
  /// succeeds — if the consumer is too far behind the oldest
  /// unread data is silently overwritten.
  public func write(source: UnsafePointer<Float>, count: Int, stride: Int = 1) {
    guard count > 0 else { return }
    var src = source
    var cnt = count
    if cnt > capacity {
      let skip = cnt - capacity
      src += skip * stride
      cnt = capacity
    }
    let w = writeIndex.load(ordering: .relaxed)
    let writeOffset = Int(w & UInt64(mask))
    let firstChunk = Swift.min(capacity - writeOffset, cnt)
    if stride == 1 {
      (base + writeOffset).update(from: src, count: firstChunk)
      if firstChunk < cnt {
        base.update(
          from: src + firstChunk,
          count: cnt - firstChunk)
      }
    } else {
      // Strided copy: extract every `stride`-th element of `source`
      // into the contiguous ring slot. `vDSP_vsadd` with a zero
      // scalar is a stride-aware copy — there's no dedicated
      // strided memcpy in vDSP.
      var zero: Float = 0
      vDSP_vsadd(
        src, vDSP_Stride(stride), &zero,
        base + writeOffset, 1,
        vDSP_Length(firstChunk))
      if firstChunk < cnt {
        vDSP_vsadd(
          src + (stride * firstChunk), vDSP_Stride(stride), &zero,
          base, 1,
          vDSP_Length(cnt - firstChunk))
      }
    }
    writeIndex.store(w &+ UInt64(cnt), ordering: .releasing)
  }

  /// **Producer-only.** Convert `count` `Double` samples from
  /// `source` to `Float` in a single `vDSP_vdpsp` call and write
  /// into the ring. Used by the spectrum-analyzer tap, which feeds
  /// engine-precision `Double` samples into a half-precision ring
  /// to halve memory.
  public func appendConvertingDoubleToFloat(_ source: UnsafePointer<Double>, count: Int) {
    guard count > 0 else { return }
    var src = source
    var cnt = count
    if cnt > capacity {
      let skip = cnt - capacity
      src += skip
      cnt = capacity
    }
    let w = writeIndex.load(ordering: .relaxed)
    let writeOffset = Int(w & UInt64(mask))
    let firstChunk = Swift.min(capacity - writeOffset, cnt)
    // vDSP_vdpsp: convert and store Double→Float, no allocation.
    vDSP_vdpsp(src, 1, base + writeOffset, 1, vDSP_Length(firstChunk))
    if firstChunk < cnt {
      let remaining = cnt - firstChunk
      vDSP_vdpsp(src + firstChunk, 1, base, 1, vDSP_Length(remaining))
    }
    writeIndex.store(w &+ UInt64(cnt), ordering: .releasing)
  }

  /// **Producer-only.** Write `count` zeros into the ring.
  /// Always succeeds — if the consumer is too far behind the oldest
  /// unread data is silently overwritten.
  public func writeSilence(count: Int) {
    guard count > 0 else { return }
    var cnt = count
    if cnt > capacity {
      cnt = capacity
    }
    let w = writeIndex.load(ordering: .relaxed)
    let writeOffset = Int(w & UInt64(mask))
    let firstChunk = Swift.min(capacity - writeOffset, cnt)

    (base + writeOffset).update(repeating: 0, count: firstChunk)
    if firstChunk < cnt {
      base.update(repeating: 0, count: cnt - firstChunk)
    }
    writeIndex.store(w &+ UInt64(cnt), ordering: .releasing)
  }

  // MARK: Consumer (consume style)

  /// **Consumer-only.** Copy up to `count` samples into `dest` and
  /// advance the read cursor. Returns the number of samples
  /// actually copied — may be less than `count` if fewer are
  /// available, in which case the remainder of `dest` is left
  /// untouched and the caller should fill it with silence.
  @discardableResult
  public func consume(into dest: UnsafeMutablePointer<Float>, count: Int) -> Int {
    guard count > 0 else { return 0 }
    var r = readIndex.load(ordering: .relaxed)
    let w = writeIndex.load(ordering: .acquiring)
    let avail = Int(w &- r)

    if avail > capacity {
      // Producer has overwritten unread data. Advance read pointer to the
      // oldest valid data (writeIndex - capacity).
      r = w - UInt64(capacity)
      readIndex.store(r, ordering: .releasing)
    }

    let n = Swift.min(Int(w &- r), count)
    guard n > 0 else { return 0 }
    let readOffset = Int(r & UInt64(mask))
    let firstChunk = Swift.min(capacity - readOffset, n)
    dest.update(from: base + readOffset, count: firstChunk)
    if firstChunk < n {
      (dest + firstChunk).update(
        from: base,
        count: n - firstChunk)
    }
    readIndex.store(r &+ UInt64(n), ordering: .releasing)
    return n
  }

  /// **Consumer-only.** Discard any pending samples without
  /// copying. Useful when the consumer wants to re-sync after a
  /// long stall.
  public func drain() {
    let w = writeIndex.load(ordering: .acquiring)
    readIndex.store(w, ordering: .releasing)
  }

  // MARK: Consumer (snapshot style)

  /// **Consumer.** Copy the most recent `count` samples into
  /// `dest` *without* advancing any cursor — subsequent calls
  /// can re-read overlapping windows. Returns `false` (without
  /// writing to `dest`) when fewer than `count` samples have been
  /// written so far.
  ///
  /// Tearing: in principle the producer can wrap the entire buffer
  /// during the consumer's memcpy. With the spectrum analyzer's
  /// 262 144-sample buffer at 48 kHz that's ~5.5 s of audio
  /// headroom — orders of magnitude longer than the consumer
  /// takes — so the snapshot is effectively atomic and we don't
  /// pay for a seqlock retry loop.
  public func readLatest(into dest: UnsafeMutablePointer<Float>, count: Int) -> Bool {
    guard count > 0, count <= capacity else { return false }
    let written = writeIndex.load(ordering: .acquiring)
    guard written >= UInt64(count) else { return false }
    let endIdx = Int(written & UInt64(mask))
    let startIdx = (endIdx + capacity - count) & mask
    let firstChunk = Swift.min(capacity - startIdx, count)
    dest.update(from: base + startIdx, count: firstChunk)
    if firstChunk < count {
      (dest + firstChunk).update(from: base, count: count - firstChunk)
    }
    return true
  }

  static func roundUpToPowerOfTwo(_ n: Int) -> Int {
    var v = 1
    while v < n { v <<= 1 }
    return v
  }
}

// MARK: - SPSC chunk queue

/// Lock-free single-producer / single-consumer FIFO queue of values of
/// arbitrary type `T`. Used to pass `AudioChunk` values between the
/// capture, processing, and playback threads inside `DSPEngineCore`
/// without taking an `NSLock`.
///
/// Power-of-two capacity. Slots store `Optional<T>` so the consumer
/// can clear back to `nil` on dequeue, dropping the ARC retain on
/// any class fields the value contains.
public final class SPSCQueue<T>: @unchecked Sendable {

  public let capacity: Int

  private let mask: Int
  /// Slot storage. Each slot holds `nil` when empty; the producer
  /// fills it on enqueue and the consumer clears it back to `nil` on
  /// dequeue (so any `T` reference fields drop their ARC retain when
  /// the consumer is done).
  private let storage: UnsafeMutableBufferPointer<T?>
  private let writeIndex = Atomic<UInt64>(0)
  private let readIndex = Atomic<UInt64>(0)

  public init(minimumCapacity: Int) {
    let cap = SPSCAudioRingBuffer.roundUpToPowerOfTwo(Swift.max(2, minimumCapacity))
    self.capacity = cap
    self.mask = cap - 1
    let buf = UnsafeMutableBufferPointer<T?>.allocate(capacity: cap)
    buf.initialize(repeating: nil)
    self.storage = buf
  }

  deinit {
    // Releasing each slot drops the ARC retain on any contained
    // references; deinitialize then deallocate the raw storage.
    for i in 0..<storage.count { storage[i] = nil }
    storage.deinitialize()
    storage.deallocate()
  }

  /// Number of currently-queued items. Approximate when read from a
  /// thread that is neither the producer nor the consumer.
  public var count: Int {
    let w = writeIndex.load(ordering: .acquiring)
    let r = readIndex.load(ordering: .relaxed)
    return Int(w &- r)
  }

  /// **Producer-only.** Append `value`; returns `false` (without
  /// storing it) when the queue is at capacity. The caller decides
  /// what to do — drop, log, or retry.
  public func enqueue(_ value: sending T) -> Bool {
    let w = writeIndex.load(ordering: .relaxed)
    let r = readIndex.load(ordering: .acquiring)
    if w &- r >= UInt64(capacity) { return false }
    storage[Int(w & UInt64(mask))] = value
    writeIndex.store(w &+ 1, ordering: .releasing)
    return true
  }

  /// **Consumer-only.** Pop the next item; returns `nil` when empty.
  public func dequeue() -> sending T? {
    let r = readIndex.load(ordering: .relaxed)
    let w = writeIndex.load(ordering: .acquiring)
    if r == w { return nil }
    let slot = Int(r & UInt64(mask))
    let value = storage[slot]
    storage[slot] = nil
    readIndex.store(r &+ 1, ordering: .releasing)
    return value
  }

  /// **Consumer-only.** Discard all queued items.
  public func drain() {
    while dequeue() != nil {}
  }
}

// MARK: - Atomic Double via UInt64 bit-pattern

/// Lock-free atomic `Double`. Swift's `Atomic` doesn't directly support
/// floating-point payloads, so we round-trip through the IEEE-754 bit
/// pattern via `UInt64`. Aligned 64-bit loads and stores are atomic on
/// every platform Swift 6 runs on, so this is genuinely wait-free.
///
/// Implicitly `Sendable` — the only stored property is `Atomic<UInt64>`,
/// which is itself `Sendable`, so the compiler infers the conformance
/// without `@unchecked`.
public final class AtomicDouble: Sendable {
  private let bits: Atomic<UInt64>

  public init(_ value: Double) {
    self.bits = Atomic<UInt64>(value.bitPattern)
  }

  public var value: Double {
    get { Double(bitPattern: bits.load(ordering: .acquiring)) }
    set { bits.store(newValue.bitPattern, ordering: .releasing) }
  }
}
