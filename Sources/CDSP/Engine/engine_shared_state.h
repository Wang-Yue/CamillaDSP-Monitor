#ifndef CLIB_ENGINE_ENGINE_SHARED_STATE_H
#define CLIB_ENGINE_ENGINE_SHARED_STATE_H

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

#include "Audio/lock_free_ring_buffer.h"
#include "Audio/audio_chunk.h"
#include <dispatch/dispatch.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Genuinely `Sendable` — every stored field is itself `Sendable`
/// (the SPSC queues, the kernel `DispatchSemaphore`s, and the
/// atomics). Producers and consumers may freely access these from
/// any thread without coordination beyond what each individual
/// field's API requires.
typedef struct {
    /// Bounded SPSC FIFO from the capture thread to the processing
    /// thread. `enqueue` returns `false` when full; the producer drops
    /// the chunk rather than allocate.
    spsc_queue_t* captured_queue;

    /// Bounded SPSC FIFO from the processing thread to the playback
    /// thread.
    spsc_queue_t* processed_queue;

    /// Wakeup signal for the processing thread. The capture thread
    /// signals after every successful `enqueue`.
    dispatch_semaphore_t captured_semaphore;

    /// Wakeup signal for the playback thread. The processing thread
    /// signals after every successful `enqueue`.
    dispatch_semaphore_t processed_semaphore;

    /// Stop flag. Written exactly once (false → true) per engine run.
    /// Each loop polls between iterations and exits when set.
    _Atomic bool should_stop;

    /// Resampler relative-ratio (≈ 1.0). Published by the playback
    /// thread (rate-adjust controller); consumed by the processing
    /// thread once per chunk via `setRelativeRatio`.
    atomic_double_t* resampler_ratio;

    /// Monotonic count of chunks dropped at the capture→processing
    /// boundary because `capturedQueue` was full. Bumped from the
    /// audio thread without formatting; observed by the actor.
    _Atomic uint64_t captured_drop_counter;
} engine_shared_state_t;

engine_shared_state_t* engine_shared_state_create(size_t captured_queue_depth, size_t processed_queue_depth);
void engine_shared_state_free(engine_shared_state_t* state);

#ifdef __cplusplus
}
#endif

#endif // CLIB_ENGINE_ENGINE_SHARED_STATE_H
