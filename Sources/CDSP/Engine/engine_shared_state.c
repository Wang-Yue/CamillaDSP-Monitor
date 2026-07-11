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
//
// `DispatchSemaphore` is included to be transparent: a semaphore is a
// kernel signaling primitive, not a lock. Producers signal after
// enqueue; consumers wait, then drain. There is never a critical
// section — a single signal can wake the consumer for any number of
// queued items, and the consumer drains until empty before waiting
// again.

#include "engine_shared_state.h"

#include <stdlib.h>

#include "Engine/engine_processing_loop.h"
#include "Pipeline/pipeline.h"

engine_shared_state_t* engine_shared_state_create(
    size_t captured_queue_depth, size_t processed_queue_depth) {
  engine_shared_state_t* state =
      (engine_shared_state_t*)calloc(1, sizeof(engine_shared_state_t));
  if (!state) return NULL;

  state->captured_queue =
      spsc_queue_create(captured_queue_depth > 0 ? captured_queue_depth : 16);
  state->processed_queue =
      spsc_queue_create(processed_queue_depth > 0 ? processed_queue_depth : 16);
  engine_sem_init(&state->captured_semaphore);
  engine_sem_init(&state->processed_semaphore);
  atomic_init(&state->should_stop, false);
  atomic_init(&state->stop_reason_written, false);
  atomic_init(&state->capture_finished, false);
  atomic_init(&state->processing_finished, false);
  state->resampler_ratio = atomic_double_create(1.0);
  state->pipeline_garbage_queue = spsc_queue_create(32);

  if (!state->captured_queue || !state->processed_queue ||
      !state->captured_semaphore || !state->processed_semaphore ||
      !state->resampler_ratio || !state->pipeline_garbage_queue) {
    engine_shared_state_free(state);
    return NULL;
  }
  return state;
}

void engine_shared_state_free(engine_shared_state_t* state) {
  if (!state) return;
  if (state->captured_queue) spsc_queue_free(state->captured_queue);
  if (state->processed_queue) spsc_queue_free(state->processed_queue);
  engine_sem_destroy(&state->captured_semaphore);
  engine_sem_destroy(&state->processed_semaphore);
  if (state->resampler_ratio) atomic_double_free(state->resampler_ratio);

  if (state->pipeline_garbage_queue) {
    void* p;
    while ((p = spsc_queue_dequeue(state->pipeline_garbage_queue)) != NULL) {
      pipeline_free((pipeline_t*)p);
    }
    spsc_queue_free(state->pipeline_garbage_queue);
  }

  free(state);
}

void engine_shared_state_request_stop(engine_shared_state_t* state,
                                      processing_stop_reason_t reason) {
  if (!state) return;
  bool expected = false;
  // Atomically check if the stop reason has already been recorded.
  // We use a first-reason-wins strategy to preserve the root cause of the stop.
  if (atomic_compare_exchange_strong(&state->stop_reason_written, &expected,
                                     true)) {
    state->stop_reason = reason;
  }
  // Store should_stop with release ordering so the write becomes visible
  // to the capture, processing, and playback loops immediately on their next
  // check.
  atomic_store_explicit(&state->should_stop, true, memory_order_release);

  // Wake up processing and playback threads if they are blocked waiting.
  engine_sem_signal(state->captured_semaphore);
  engine_sem_signal(state->processed_semaphore);
}
