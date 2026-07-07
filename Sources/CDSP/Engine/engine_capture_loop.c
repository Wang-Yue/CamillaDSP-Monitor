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
#include "engine_capture_loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "Logging/app_logger.h"
#include "thread_priority.h"

#ifndef __APPLE__
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
static inline uint64_t clock_gettime_nsec_np(int clock_id) {
  struct timespec ts;
  clock_gettime(clock_id, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
#endif

engine_capture_loop_t* engine_capture_loop_create(
    engine_shared_state_t* shared, engine_state_machine_t* state_machine,
    capture_backend_t* capture, playback_backend_t* playback,
    processing_parameters_t* processing_params, dop_decoder_t* dop_decoder,
    size_t chunk_size, size_t channels, size_t samplerate,
    double silence_threshold_db, double silence_timeout_seconds,
    engine_stop_callback_t on_stop, void* on_stop_ctx) {
  engine_capture_loop_t* loop =
      (engine_capture_loop_t*)calloc(1, sizeof(engine_capture_loop_t));
  if (!loop) return NULL;

  loop->shared = shared;
  loop->state_machine = state_machine;
  loop->capture = capture;
  loop->playback = playback;
  loop->processing_params = processing_params;
  loop->dop_decoder = dop_decoder;
  loop->chunk_size = chunk_size;
  loop->channels = channels;
  loop->samplerate = samplerate;
  loop->on_stop = on_stop;
  loop->on_stop_ctx = on_stop_ctx;

  silence_counter_init(&loop->silence_counter, silence_threshold_db,
                       silence_timeout_seconds, samplerate, chunk_size);
  loop->watchdog_timeout_seconds = 0.5;
  loop->watchdog_last_success_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  loop->watchdog_triggered = false;

  return loop;
}

void engine_capture_loop_free(engine_capture_loop_t* loop) {
  if (!loop) return;
  free(loop);
}

void engine_capture_loop_run(engine_capture_loop_t* loop) {
  if (!loop) return;
  logger_t logger = logger_create("dsp.capture");
  logger_info(&logger, "Capture thread started", log_arg_none(), log_arg_none(),
              log_arg_none(), log_arg_none());

  set_realtime_thread_priority("Capture", loop->chunk_size, loop->samplerate);

  size_t pool_cap = loop->shared->captured_queue->capacity + 4;
  round_robin_chunk_pool_t* chunk_pool =
      round_robin_chunk_pool_create(pool_cap, loop->chunk_size, loop->channels);

  while (
      !atomic_load_explicit(&loop->shared->should_stop, memory_order_acquire)) {
    // Surface a HAL-level sample-rate change before doing any
    // more work. A user (or another app) flipping the device
    // rate in Audio MIDI Setup invalidates the AudioUnit's
    // configured format; the cleanest recovery is to stop
    // unconditionally and let the host rebuild.
    double rate = 0.0;
    if (capture_backend_get_pending_rate_change(loop->capture, &rate)) {
      if (!loop->has_last_observed_pending_rate ||
          rate != loop->last_observed_pending_rate) {
        loop->last_observed_pending_rate = rate;
        loop->has_last_observed_pending_rate = true;
        logger_warn(&logger,
                    "Capture device rate changed to %f Hz; stopping engine",
                    log_arg_double(rate), log_arg_none(), log_arg_none(),
                    log_arg_none());
        processing_stop_reason_t reason = {
            .type = STOP_REASON_CAPTURE_FORMAT_CHANGE,
            .format_change_rate = (int)(rate + 0.5)};
        if (loop->on_stop) loop->on_stop(loop->on_stop_ctx, reason);
        break;
      }
    }
    if (playback_backend_get_pending_rate_change(loop->playback, &rate)) {
      if (!loop->has_last_observed_playback_pending_rate ||
          rate != loop->last_observed_playback_pending_rate) {
        loop->last_observed_playback_pending_rate = rate;
        loop->has_last_observed_playback_pending_rate = true;
        logger_warn(&logger,
                    "Playback device rate changed to %f Hz; stopping engine",
                    log_arg_double(rate), log_arg_none(), log_arg_none(),
                    log_arg_none());
        processing_stop_reason_t reason = {
            .type = STOP_REASON_PLAYBACK_FORMAT_CHANGE,
            .format_change_rate = (int)(rate + 0.5)};
        if (loop->on_stop) loop->on_stop(loop->on_stop_ctx, reason);
        break;
      }
    }

    audio_chunk_t* chunk = round_robin_chunk_pool_next(chunk_pool);
    backend_error_t err;
    backend_error_init(&err, BACKEND_ERROR_NONE, "");
    bool got_data =
        capture_backend_read(loop->capture, loop->chunk_size, chunk, &err);
    if (!got_data) {
      if (err.type != BACKEND_ERROR_NONE) {
        static char s_capture_err_log[256];
        snprintf(s_capture_err_log, sizeof(s_capture_err_log), "%s",
                 err.message);
        logger_error(&logger, "Capture error: %s",
                     log_arg_string(s_capture_err_log), log_arg_none(),
                     log_arg_none(), log_arg_none());
        processing_stop_reason_t reason = {.type = STOP_REASON_CAPTURE_ERROR};
        snprintf(reason.message, sizeof(reason.message), "%s", err.message);
        if (loop->on_stop) loop->on_stop(loop->on_stop_ctx, reason);
        break;
      }
      if (atomic_load_explicit(&loop->shared->should_stop,
                               memory_order_acquire))
        break;
      if (engine_state_machine_get_state(loop->state_machine) ==
          PROCESSING_STATE_PAUSED) {
        loop->watchdog_last_success_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        capture_backend_wait(loop->capture, 20);
        continue;
      }
      if (!loop->watchdog_triggered) {
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        double elapsed =
            (double)(now - loop->watchdog_last_success_ns) / 1000000000.0;
        if (elapsed > loop->watchdog_timeout_seconds) {
          loop->watchdog_triggered = true;
          engine_state_machine_set_state(loop->state_machine,
                                         PROCESSING_STATE_STALLED);
          logger_warn(&logger, "Capture device stalled — no data for %fs",
                      log_arg_double(loop->watchdog_timeout_seconds),
                      log_arg_none(), log_arg_none(), log_arg_none());
        }
      }
      // Wait on the capture device's GCD semaphore for new samples, up to 20ms.
      // This uses a 20ms timeout design, preserving
      // real-time priority propagation under load instead of doing a raw sleep.
      capture_backend_wait(loop->capture, 20);
      continue;
    }

    loop->watchdog_last_success_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (loop->watchdog_triggered) {
      loop->watchdog_triggered = false;
      logger_info(&logger, "Capture recovered from stall", log_arg_none(),
                  log_arg_none(), log_arg_none(), log_arg_none());
    }

    // Decode DoP in place before computing capture levels so the
    // monitoring meters reflect the actual decoded audio rather
    // than the carrier waveform with its high-frequency marker
    // bytes (which would otherwise show a tiny ~0.04 amplitude
    // floor).
    if (loop->dop_decoder) {
      dop_decoder_detect_and_process(loop->dop_decoder, chunk);
    }

    double loudest_peak = processing_parameters_update_capture_levels(
        loop->processing_params, chunk);

    // Update silence detector with the loudest channel's peak.
    // We only flip when the value actually changes to avoid
    // hammering the atomic from the audio thread.
    processing_state_t desired =
        silence_counter_update(&loop->silence_counter, loudest_peak);
    processing_state_t current =
        engine_state_machine_get_state(loop->state_machine);
    if (desired != current) {
      engine_state_machine_set_state(loop->state_machine, desired);
      playback_backend_set_is_paused(loop->playback,
                                     (desired == PROCESSING_STATE_PAUSED));
      capture_backend_set_is_paused(loop->capture,
                                    (desired == PROCESSING_STATE_PAUSED));
    }

    // Enqueue for processing. The lock-free SPSC queue is
    // bounded; on overflow we drop the chunk rather than
    // allocate. We bump an atomic counter instead of calling
    // the logger — formatting / locking inside the logger is
    // a poor fit for the audio-priority capture thread, and
    // particularly bad precisely when the system is already
    // overloaded.
    if (engine_state_machine_get_state(loop->state_machine) !=
        PROCESSING_STATE_PAUSED) {
      if (!spsc_queue_enqueue(loop->shared->captured_queue, chunk)) {
        atomic_fetch_add_explicit(&loop->shared->captured_drop_counter, 1,
                                  memory_order_relaxed);
      }
      engine_sem_signal(loop->shared->captured_semaphore);
    }
  }

  round_robin_chunk_pool_free(chunk_pool);
  logger_info(&logger, "Capture thread stopped", log_arg_none(), log_arg_none(),
              log_arg_none(), log_arg_none());
}
