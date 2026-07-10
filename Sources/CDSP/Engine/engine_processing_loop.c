// Processing thread body. Drains the capture→processing SPSC queue,
// runs each chunk through the (optional) resampler and the pipeline,
// then enqueues the result on the processing→playback queue.
//
// State ownership
// ---------------
// The pre-allocated scratch chunks (`resamplerScratch`,
// `pipelineScratch`) are owned by this loop and only mutated here.
// The resampler's own internal state is also single-threaded: the
// playback thread publishes a relative ratio via the shared atomic,
// and the processing thread consumes it once per chunk through
// `setRelativeRatio`. No cross-thread mutation of resampler state.
//
// Audio-thread invariants
// -----------------------
//   * No allocations in the steady state. Output chunks are obtained
//     from a pre-allocated `RoundRobinChunkPool`, and the resampler
//     scratch chunk is pre-allocated at init.
//   * No locks. The shared SPSC queues + semaphores carry chunks
//     and wakeups; the resampler ratio is an atomic Double.
//   * The thread sets a real-time scheduling policy on entry so the
//     OS prefers it over background work.
#include "engine_processing_loop.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

struct pending_update {
  dsp_config_t* config;
  char** filters;
  size_t filters_count;
  char** mixers;
  size_t mixers_count;
  char** processors;
  size_t processors_count;
};

struct engine_processing_loop {
  engine_shared_state_t* shared;
  engine_state_machine_t* state_machine;
  processing_parameters_t* processing_params;
  size_t pipeline_rate;
  audio_resampler_t* resampler;
  pipeline_t* active_pipeline;
  dop_encoder_t* dop_encoder;
  _Atomic(pipeline_t*) next_pipeline;
  audio_chunk_t* resampler_scratch;
  audio_chunk_t* pipeline_scratch;
  round_robin_chunk_pool_t* scratch_pool;

  chunk_callback_t on_chunk_captured;
  void* on_chunk_captured_ctx;
  chunk_callback_t on_chunk_processed;
  void* on_chunk_processed_ctx;
};

#include <string.h>
#include <time.h>

#include "Logging/app_logger.h"
#include "thread_priority.h"

#ifndef __APPLE__
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC

/**
 * @brief Helper to get the current time in nanoseconds for non-Apple platforms.
 *
 * Provides an equivalent to Apple's clock_gettime_nsec_np for measuring elapsed
 * time.
 *
 * @param clock_id The clock identifier to use (e.g., CLOCK_MONOTONIC).
 * @return The current time in nanoseconds.
 */
static inline uint64_t clock_gettime_nsec_np(int clock_id) {
  struct timespec ts;
  clock_gettime(clock_id, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
#endif

engine_processing_loop_t* engine_processing_loop_create(
    engine_shared_state_t* shared, engine_state_machine_t* state_machine,
    processing_parameters_t* processing_params, size_t pipeline_rate,
    audio_resampler_t* resampler, pipeline_t* pipeline,
    dop_encoder_t* dop_encoder, audio_chunk_t* resampler_scratch,
    audio_chunk_t* pipeline_scratch, round_robin_chunk_pool_t* scratch_pool,
    chunk_callback_t on_chunk_captured, void* on_chunk_captured_ctx,
    chunk_callback_t on_chunk_processed, void* on_chunk_processed_ctx) {
  engine_processing_loop_t* loop =
      (engine_processing_loop_t*)calloc(1, sizeof(engine_processing_loop_t));
  if (!loop) return NULL;
  loop->shared = shared;
  loop->state_machine = state_machine;
  loop->processing_params = processing_params;
  loop->pipeline_rate = pipeline_rate;
  loop->resampler = resampler;
  loop->active_pipeline = pipeline;
  loop->dop_encoder = dop_encoder;
  loop->resampler_scratch = resampler_scratch;
  loop->pipeline_scratch = pipeline_scratch;
  loop->scratch_pool = scratch_pool;
  loop->on_chunk_captured = on_chunk_captured;
  loop->on_chunk_captured_ctx = on_chunk_captured_ctx;
  loop->on_chunk_processed = on_chunk_processed;
  loop->on_chunk_processed_ctx = on_chunk_processed_ctx;

  atomic_store(&loop->next_pipeline, NULL);

  return loop;
}

void engine_processing_loop_free(engine_processing_loop_t* loop) {
  if (!loop) return;
  pipeline_t* p = atomic_exchange(&loop->next_pipeline, NULL);
  if (p) {
    pipeline_free(p);
  }
  if (loop->active_pipeline) {
    pipeline_free(loop->active_pipeline);
  }
  free(loop);
}

void engine_processing_loop_set_pipeline(engine_processing_loop_t* loop,
                                         pipeline_t* new_pipeline) {
  if (loop) {
    pipeline_t* old = atomic_exchange(&loop->next_pipeline, new_pipeline);
    if (old) {
      pipeline_free(old);
    }
  }
}

void engine_processing_loop_run(engine_processing_loop_t* loop) {
  if (!loop) return;
  logger_t logger = logger_create("dsp.processing");
  logger_info(&logger, "Processing thread started", log_arg_none(),
              log_arg_none(), log_arg_none(), log_arg_none());

  set_realtime_thread_priority("Processing",
                               audio_chunk_get_frames(loop->pipeline_scratch),
                               loop->pipeline_rate);

  while (1) {
    engine_sem_wait(loop->shared->captured_semaphore);

    bool emergency = atomic_load_explicit(&loop->shared->should_stop,
                                          memory_order_acquire) &&
                     loop->shared->stop_reason.type != STOP_REASON_DONE;
    bool graceful = atomic_load_explicit(&loop->shared->capture_finished,
                                         memory_order_acquire) &&
                    spsc_queue_get_count(loop->shared->captured_queue) == 0;
    if (emergency || graceful) {
      break;
    }

    // Drain everything the capture thread enqueued since the last
    // wake. One semaphore signal can correspond to multiple
    // enqueues if the producer outran us briefly; the inner loop
    // catches up before we wait again.
    audio_chunk_t* chunk = NULL;
    while ((chunk = (audio_chunk_t*)spsc_queue_dequeue(
                loop->shared->captured_queue)) != NULL) {
      uint64_t res_start = 0;
      uint64_t res_end = 0;
      if (loop->resampler) {
        // Resample if configured. The desired ratio is published
        // by the rate-adjust controller via `shared.resamplerRatio`;
        // we sync the resampler to it once per chunk. The
        // resampler's internal state is otherwise owned exclusively
        // by this thread, so no lock is required.
        double ratio = atomic_double_get(loop->shared->resampler_ratio);
        audio_resampler_set_relative_ratio(loop->resampler, ratio);

        // Write into the pre-sized output scratch (sized to
        // `resampler.maxOutputFrames`), then make that scratch
        // our working chunk. We can't `swap` here — a non-1:1
        // resampler has different input/output chunk sizes, so
        // swapping would leave scratch holding a too-small array
        // on the next iteration.
        res_start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        resampler_error_t rerr = audio_resampler_process(
            loop->resampler, chunk, loop->resampler_scratch);
        res_end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (rerr != RESAMPLER_OK) {
          logger_error(&logger, "Processing error: resampler error %d",
                       log_arg_int((int64_t)rerr), log_arg_none(),
                       log_arg_none(), log_arg_none());
          processing_stop_reason_t reason = {.type = STOP_REASON_UNKNOWN_ERROR};
          snprintf(reason.message, sizeof(reason.message), "Resampler error %d",
                   rerr);
          engine_shared_state_request_stop(loop->shared, reason);
          return;
        }
        chunk = loop->resampler_scratch;
      }

      // Pre-processing tap for visualisation.
      if (loop->on_chunk_captured) {
        loop->on_chunk_captured(loop->on_chunk_captured_ctx, chunk);
      }

      // Run through the pipeline using pre-allocated output
      // scratch.
      pipeline_t* next_pipeline = atomic_exchange(&loop->next_pipeline, NULL);
      if (next_pipeline) {
        if (loop->active_pipeline) {
          pipeline_transfer_state(next_pipeline, loop->active_pipeline);
          if (!spsc_queue_enqueue(loop->shared->pipeline_garbage_queue,
                                  loop->active_pipeline)) {
            pipeline_free(loop->active_pipeline);
          }
        }
        loop->active_pipeline = next_pipeline;
      }

      if (engine_state_machine_get_state(loop->state_machine) ==
          PROCESSING_STATE_PAUSED) {
        continue;
      }

      // Retrieve a pre-allocated scratch chunk from the round-robin pool.
      // We must use a pool because the pipeline output chunk is passed down the
      // queue to the playback thread, and we cannot reuse it immediately.
      audio_chunk_t* current_scratch =
          round_robin_chunk_pool_next(loop->scratch_pool);
      uint64_t pipe_start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
      pipeline_error_t perr =
          pipeline_process(loop->active_pipeline, chunk, current_scratch);
      uint64_t pipe_end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
      if (perr != PIPELINE_OK) {
        logger_error(&logger, "Processing error: pipeline error %d",
                     log_arg_int((int64_t)perr), log_arg_none(), log_arg_none(),
                     log_arg_none());
        processing_stop_reason_t reason = {.type = STOP_REASON_UNKNOWN_ERROR};
        snprintf(reason.message, sizeof(reason.message), "Pipeline error %d",
                 perr);
        engine_shared_state_request_stop(loop->shared, reason);
        return;
      }
      chunk = current_scratch;

      // Calculate CPU load of the DSP pipeline and resampler.
      // The load is the ratio of processing time (in nanoseconds) to the
      // physical duration of the audio chunk. A load > 1.0 means we cannot
      // process in real-time and will cause dropouts.
      if (loop->processing_params) {
        size_t frames = audio_chunk_get_valid_frames(chunk);
        if (frames > 0) {
          uint64_t chunk_duration_ns =
              (uint64_t)frames * 1000000000ULL / loop->pipeline_rate;
          if (chunk_duration_ns > 0) {
            double p_load =
                (double)(pipe_end - pipe_start) / (double)chunk_duration_ns;
            atomic_double_set(&loop->processing_params->processing_load,
                              p_load);

            if (loop->resampler) {
              double r_load =
                  (double)(res_end - res_start) / (double)chunk_duration_ns;
              atomic_double_set(&loop->processing_params->resampler_load,
                                r_load);
            } else {
              atomic_double_set(&loop->processing_params->resampler_load, 0.0);
            }
          }
        }

        // Scan the output chunk for clipped samples (outside [-1.0, 1.0]
        // range). This is done before DoP encoding.
        size_t channels = audio_chunk_get_channels(chunk);
        size_t c_frames = audio_chunk_get_valid_frames(chunk);
        uint64_t clipped = 0;
        for (size_t c = 0; c < channels; c++) {
          mutable_waveform_t data = audio_chunk_get_channel(chunk, c);
          for (size_t f = 0; f < c_frames; f++) {
            if (data[f] > 1.0 || data[f] < -1.0) {
              clipped++;
            }
          }
        }
        if (clipped > 0) {
          atomic_fetch_add_explicit(&loop->processing_params->clipped_samples,
                                    clipped, memory_order_relaxed);
        }
      }

      processing_parameters_update_playback_levels(loop->processing_params,
                                                   chunk);

      if (loop->on_chunk_processed) {
        loop->on_chunk_processed(loop->on_chunk_processed_ctx, chunk);
      }

      // Encode PCM to DoP in place if enabled
      if (loop->dop_encoder) {
        dop_encoder_encode(loop->dop_encoder, chunk);
      }

      // Enqueue the processed chunk to the playback queue.
      // If the queue is full (playback thread falling behind), we block-wait
      // using a short sleep to avoid spinning and wasting CPU.
      while (!spsc_queue_enqueue(loop->shared->processed_queue, chunk)) {
        if (atomic_load_explicit(&loop->shared->should_stop,
                                 memory_order_acquire) &&
            loop->shared->stop_reason.type != STOP_REASON_DONE) {
          break;
        }
        engine_yield();
      }
      engine_sem_signal(loop->shared->processed_semaphore);
    }
  }

  // Propagate graceful shutdown sequentially.
  atomic_store_explicit(&loop->shared->processing_finished, true,
                        memory_order_release);
  engine_sem_signal(loop->shared->processed_semaphore);

  logger_info(&logger, "Processing thread stopped", log_arg_none(),
              log_arg_none(), log_arg_none(), log_arg_none());
}
