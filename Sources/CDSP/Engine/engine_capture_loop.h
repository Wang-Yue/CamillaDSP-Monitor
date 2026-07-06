#ifndef CLIB_ENGINE_ENGINE_CAPTURE_LOOP_H
#define CLIB_ENGINE_ENGINE_CAPTURE_LOOP_H

// Capture thread body. One instance per engine run; the thread
// closure invokes `run()` exactly once and returns when the shared
// `shouldStop` flag is set or a stop reason is reported.
//
// State ownership
// ---------------
// All mutable state — the working chunk, the silence counter, the
// stall watchdog — lives inside the loop instance and is touched
// only by the capture thread. Cross-thread communication happens
// exclusively through the injected `EngineSharedState`.
//
// Audio-thread invariants
// -----------------------
//   * No allocations in the steady-state. Audio chunks are obtained
//     from a pre-allocated `RoundRobinChunkPool`.
//   * No locks. Coordination uses the shared SPSC queue + semaphore.
//   * No `Date()` / `gettimeofday`. The watchdog uses
//     `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` (vDSO read on
//     Darwin — no syscall).

#include <stdbool.h>
#include <stddef.h>

#include "Audio/processing_parameters.h"
#include "Audio/silence_counter.h"
#include "Backend/audio_backend.h"
#include "DoP/dop_decoder.h"
#include "engine_shared_state.h"
#include "engine_state_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

/// `@unchecked Sendable` is a *transfer* vouch, not a *share*
/// vouch: the instance is safe to cross the Thread spawn boundary
/// because exactly one thread (the loop thread) ever touches it
/// after `run()` is invoked. The mutable state — the working
/// `AudioChunk`, the silence counter, the stall watchdog — has no
/// internal synchronisation and is *not* safe to use from multiple
/// threads concurrently.
typedef struct {
  engine_shared_state_t* shared;
  engine_state_machine_t* state_machine;
  capture_backend_t* capture;
  playback_backend_t* playback;
  processing_parameters_t* processing_params;
  dop_decoder_t* dop_decoder;

  size_t chunk_size;
  size_t channels;
  size_t samplerate;
  double last_observed_pending_rate;
  bool has_last_observed_pending_rate;
  double last_observed_playback_pending_rate;
  bool has_last_observed_playback_pending_rate;

  /// Hooked stop callback. Invoked when capture decides the engine
  /// must shut down (format change / capture error / stall). The
  /// host wires this to `DSPEngineCore.stop(reason:)` so the once-CAS
  /// teardown runs exactly once even when several signals fire
  /// concurrently.
  engine_stop_callback_t on_stop;
  void* on_stop_ctx;

  // Loop-private state.

  // MARK: - SilenceCounter
  /// Counts consecutive silent chunks against a dB threshold and
  /// reports back the desired engine state. `update(signalPeakDb:)`
  /// returns `.paused` once silence has persisted for at least the
  /// configured timeout, `.running` otherwise.
  ///
  /// Disabled when `timeoutSeconds <= 0` — in that case `update`
  /// always returns `.running`.
  silence_counter_t silence_counter;

  // MARK: - StallWatchdog
  /// Detects a hung capture device — `read` returning no data for
  /// longer than `timeoutSeconds` consecutively. The watchdog records
  /// the monotonic time of the most recent successful read and reports
  /// `true` exactly once per stall (subsequent ticks return `false`
  /// until the next successful read clears the flag).
  ///
  /// Backed by `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` — a vDSO
  /// read on Darwin, no syscall, suitable for invocation on every
  /// audio-thread iteration.
  uint64_t watchdog_last_success_ns;
  bool watchdog_triggered;
  double watchdog_timeout_seconds;
} engine_capture_loop_t;

engine_capture_loop_t* engine_capture_loop_create(
    engine_shared_state_t* shared, engine_state_machine_t* state_machine,
    capture_backend_t* capture, playback_backend_t* playback,
    processing_parameters_t* processing_params, dop_decoder_t* dop_decoder,
    size_t chunk_size, size_t channels, size_t samplerate,
    double silence_threshold_db, double silence_timeout_seconds,
    engine_stop_callback_t on_stop, void* on_stop_ctx);

void engine_capture_loop_free(engine_capture_loop_t* loop);
void engine_capture_loop_run(engine_capture_loop_t* loop);

#ifdef __cplusplus
}
#endif

#endif  // CLIB_ENGINE_ENGINE_CAPTURE_LOOP_H
