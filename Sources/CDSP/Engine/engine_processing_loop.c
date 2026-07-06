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
#if defined(__linux__)
#define _GNU_SOURCE
#endif
#include "engine_processing_loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

engine_processing_loop_t* engine_processing_loop_create(
    engine_shared_state_t* shared, engine_state_machine_t* state_machine,
    processing_parameters_t* processing_params, size_t pipeline_rate,
    audio_resampler_t* resampler, pipeline_t* pipeline,
    dop_encoder_t* dop_encoder, audio_chunk_t* resampler_scratch,
    audio_chunk_t* pipeline_scratch, chunk_callback_t on_chunk_captured,
    void* on_chunk_captured_ctx, chunk_callback_t on_chunk_processed,
    void* on_chunk_processed_ctx, engine_stop_callback_t on_stop,
    void* on_stop_ctx) {
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
  loop->on_chunk_captured = on_chunk_captured;
  loop->on_chunk_captured_ctx = on_chunk_captured_ctx;
  loop->on_chunk_processed = on_chunk_processed;
  loop->on_chunk_processed_ctx = on_chunk_processed_ctx;
  loop->on_stop = on_stop;
  loop->on_stop_ctx = on_stop_ctx;

  loop->pipeline_queue = spsc_queue_create(2);
  loop->update_queue = spsc_queue_create(8);

  return loop;
}

void engine_processing_loop_free(engine_processing_loop_t* loop) {
  if (!loop) return;
  if (loop->pipeline_queue) {
    pipeline_t* p = NULL;
    while ((p = (pipeline_t*)spsc_queue_dequeue(loop->pipeline_queue)) !=
           NULL) {
      pipeline_free(p);
    }
    spsc_queue_free(loop->pipeline_queue);
  }
  if (loop->update_queue) {
    pending_update_t* u = NULL;
    while ((u = (pending_update_t*)spsc_queue_dequeue(loop->update_queue)) !=
           NULL) {
      pending_update_free(u);
    }
    spsc_queue_free(loop->update_queue);
  }
  if (loop->active_pipeline) {
    pipeline_free(loop->active_pipeline);
  }
  free(loop);
}

void pending_update_free(pending_update_t* update) {
  if (!update) return;
  if (update->config) dsp_config_free(update->config);
  if (update->filters) {
    for (size_t i = 0; i < update->filters_count; i++) free(update->filters[i]);
    free(update->filters);
  }
  if (update->mixers) {
    for (size_t i = 0; i < update->mixers_count; i++) free(update->mixers[i]);
    free(update->mixers);
  }
  if (update->processors) {
    for (size_t i = 0; i < update->processors_count; i++)
      free(update->processors[i]);
    free(update->processors);
  }
  free(update);
}

void engine_processing_loop_set_pipeline(engine_processing_loop_t* loop,
                                         pipeline_t* new_pipeline) {
  if (loop && loop->pipeline_queue) {
    if (!spsc_queue_enqueue(loop->pipeline_queue, new_pipeline)) {
      pipeline_free(new_pipeline);
    }
  }
}

void engine_processing_loop_enqueue_update(engine_processing_loop_t* loop,
                                           pending_update_t* update) {
  if (loop && loop->update_queue) {
    if (!spsc_queue_enqueue(loop->update_queue, update)) {
      pending_update_free(update);
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

  size_t pool_cap = loop->shared->processed_queue->capacity + 4;
  round_robin_chunk_pool_t* scratch_pool = round_robin_chunk_pool_create(
      pool_cap, audio_chunk_get_frames(loop->pipeline_scratch),
      audio_chunk_get_channels(loop->pipeline_scratch));

  int processed_count = 0;

  while (
      !atomic_load_explicit(&loop->shared->should_stop, memory_order_acquire)) {
    engine_sem_wait(loop->shared->captured_semaphore);
    if (atomic_load_explicit(&loop->shared->should_stop, memory_order_acquire))
      break;

    // Drain everything the capture thread enqueued since the last
    // wake. One semaphore signal can correspond to multiple
    // enqueues if the producer outran us briefly; the inner loop
    // catches up before we wait again.
    audio_chunk_t* chunk = NULL;
    while ((chunk = (audio_chunk_t*)spsc_queue_dequeue(
                loop->shared->captured_queue)) != NULL) {
      if (atomic_load_explicit(&loop->shared->should_stop,
                               memory_order_acquire)) {
        round_robin_chunk_pool_free(scratch_pool);
        return;
      }
      processed_count++;

      // Apply any pending parameter updates before processing this chunk
      pending_update_t* update = NULL;
      while ((update = (pending_update_t*)spsc_queue_dequeue(
                  loop->update_queue)) != NULL) {
        pipeline_update_parameters(
            loop->active_pipeline, update->config,
            (const char* const*)update->filters, update->filters_count,
            (const char* const*)update->mixers, update->mixers_count,
            (const char* const*)update->processors, update->processors_count);
        pending_update_free(update);
      }

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
          if (loop->on_stop) loop->on_stop(loop->on_stop_ctx, reason);
          round_robin_chunk_pool_free(scratch_pool);
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
      pipeline_t* next_pipeline =
          (pipeline_t*)spsc_queue_dequeue(loop->pipeline_queue);
      if (next_pipeline) {
        if (loop->active_pipeline) {
          pipeline_free(loop->active_pipeline);
        }
        loop->active_pipeline = next_pipeline;
      }

      if (engine_state_machine_get_state(loop->state_machine) ==
          PROCESSING_STATE_PAUSED) {
        continue;
      }

      audio_chunk_t* current_scratch =
          round_robin_chunk_pool_next(scratch_pool);
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
        if (loop->on_stop) loop->on_stop(loop->on_stop_ctx, reason);
        round_robin_chunk_pool_free(scratch_pool);
        return;
      }
      chunk = current_scratch;

      if (loop->processing_params) {
        size_t frames = chunk->valid_frames;
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

        // Check for clipped samples on the output chunk
        size_t channels = audio_chunk_get_channels(chunk);
        size_t c_frames = chunk->valid_frames;
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

      if (!spsc_queue_enqueue(loop->shared->processed_queue, chunk)) {
        logger_warn(&logger,
                    "Playback queue full, dropping processed chunk #%d",
                    log_arg_int((int64_t)processed_count), log_arg_none(),
                    log_arg_none(), log_arg_none());
      }
      engine_sem_signal(loop->shared->processed_semaphore);
    }
  }

  round_robin_chunk_pool_free(scratch_pool);
  logger_info(&logger, "Processing thread stopped", log_arg_none(),
              log_arg_none(), log_arg_none(), log_arg_none());
}
