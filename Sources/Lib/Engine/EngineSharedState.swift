// Inter-thread state for the DSP engine's three audio-priority loops
// (capture / processing / playback). Every field here is either a
// lock-free atomic, a wait-free SPSC queue, or a kernel signaling
// primitive (`DispatchSemaphore`). No mutexes, no `NSLock`, no
// `@unchecked` reads of shared mutable references — so any of the
// three loops can read or write any of these fields without
// coordinating with the others.
//
// Concurrency model
// -----------------
//   shouldStop          — written by `stop()` / read by all three loops
//                         every iteration. Atomic<Bool> w/ release-acquire
//                         so a stop request becomes promptly visible.
//   capturedQueue       — SPSC, single producer = capture, single
//                         consumer = processing.
//   processedQueue      — SPSC, single producer = processing, single
//                         consumer = playback.
//   capturedSemaphore   — capture signals, processing waits.
//   processedSemaphore  — processing signals, playback waits.
//   resamplerRatio      — playback writes (rate-adjust controller),
//                         processing reads (per chunk). 64-bit atomic.
//   capturedDropCounter — capture writes (dropped enqueues),
//                         actor reads (monitoring). Atomic<UInt64>.
//
// `DispatchSemaphore` is included to be transparent: a semaphore is a
// kernel signaling primitive, not a lock. Producers signal after
// enqueue; consumers wait, then drain. There is never a critical
// section — a single signal can wake the consumer for any number of
// queued items, and the consumer drains until empty before waiting
// again.

import DSPAudio
import Foundation
import Synchronization

/// Genuinely `Sendable` — every stored field is itself `Sendable`
/// (the SPSC queues, the kernel `DispatchSemaphore`s, and the
/// atomics). Producers and consumers may freely access these from
/// any thread without coordination beyond what each individual
/// field's API requires.
final class EngineSharedState: Sendable {
  /// Bounded SPSC FIFO from the capture thread to the processing
  /// thread. `enqueue` returns `false` when full; the producer drops
  /// the chunk rather than allocate.
  let capturedQueue: SPSCQueue<AudioChunk>

  /// Bounded SPSC FIFO from the processing thread to the playback
  /// thread.
  let processedQueue: SPSCQueue<AudioChunk>

  /// Wakeup signal for the processing thread. The capture thread
  /// signals after every successful `enqueue`.
  let capturedSemaphore = DispatchSemaphore(value: 0)

  /// Wakeup signal for the playback thread. The processing thread
  /// signals after every successful `enqueue`.
  let processedSemaphore = DispatchSemaphore(value: 0)

  /// Stop flag. Written exactly once (false → true) per engine run.
  /// Each loop polls between iterations and exits when set.
  let shouldStop = Atomic<Bool>(false)

  /// Resampler relative-ratio (≈ 1.0). Published by the playback
  /// thread (rate-adjust controller); consumed by the processing
  /// thread once per chunk via `setRelativeRatio`.
  let resamplerRatio = AtomicDouble(1.0)

  /// Monotonic count of chunks dropped at the capture→processing
  /// boundary because `capturedQueue` was full. Bumped from the
  /// audio thread without formatting; observed by the actor.
  let capturedDropCounter = Atomic<UInt64>(0)

  init(capturedQueueDepth: Int = 16, processedQueueDepth: Int = 16) {
    self.capturedQueue = SPSCQueue<AudioChunk>(minimumCapacity: capturedQueueDepth)
    self.processedQueue = SPSCQueue<AudioChunk>(minimumCapacity: processedQueueDepth)
  }
}
