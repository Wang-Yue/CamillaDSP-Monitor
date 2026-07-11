// top-level engine orchestrator.
//
// This class owns the *shape* of an engine run — config, sizing,
// device handles, the three audio threads — but contains no audio
// processing logic itself. Each thread body lives in its own file:
//
//   * `EngineCaptureLoop`     — capture → DoP-decode → level meter
//                               → SPSC queue.
//   * `EngineProcessingLoop`  — SPSC dequeue → resample → pipeline
//                               → SPSC enqueue.
//   * `EnginePlaybackLoop`    — SPSC dequeue → rate-adjust controller
//                               → device write.
//
// All cross-thread state (the stop flag, the SPSC queues, the
// resampler-ratio atomic) lives in `EngineSharedState`. State
// machine + stop-reason publication lives in `EngineStateMachine`.
//
// Lock-free / allocation-free guarantees
// --------------------------------------
//   * The audio threads use lock-free SPSC queues and atomics;
//     only `DispatchSemaphore` is used for signal/wait, which is
//     a kernel signaling primitive (not a lock).
//   * Chunks are managed using a pre-allocated `RoundRobinChunkPool`
//     on each thread to avoid allocations on the hot path.
//   * The resampler and pipeline output scratch buffers are pre-allocated.
//   * The stall watchdog uses `clock_gettime_nsec_np` (vDSO read,
//     no syscall) — no `Date()` on the hot path.

#include "dsp_engine_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Config/config_diff.h"
#include "Logging/app_logger.h"

/**
 * @brief Thread entry point wrapper for the audio capture loop.
 *
 * @param arg Pointer to the engine_capture_loop_t instance.
 * @return NULL.
 */
static void* capture_thread_func(void* arg) {
  engine_capture_loop_t* loop = (engine_capture_loop_t*)arg;
  engine_capture_loop_run(loop);
  return NULL;
}

/**
 * @brief Thread entry point wrapper for the DSP processing pipeline loop.
 *
 * @param arg Pointer to the engine_processing_loop_t instance.
 * @return NULL.
 */
static void* processing_thread_func(void* arg) {
  engine_processing_loop_t* loop = (engine_processing_loop_t*)arg;
  engine_processing_loop_run(loop);
  return NULL;
}

/**
 * @brief Thread entry point wrapper for the audio playback loop.
 *
 * @param arg Pointer to the engine_playback_loop_t instance.
 * @return NULL.
 */
static void* playback_thread_func(void* arg) {
  engine_playback_loop_t* loop = (engine_playback_loop_t*)arg;
  engine_playback_loop_run(loop);
  return NULL;
}

// MARK: - Init

dsp_engine_core_t* dsp_engine_core_create(dsp_config_t* config) {
  if (!config) return NULL;
  dsp_engine_core_t* core =
      (dsp_engine_core_t*)calloc(1, sizeof(dsp_engine_core_t));
  if (!core) return NULL;

  core->current_config = config;
  int queue_limit =
      config->devices.has_queuelimit ? config->devices.queuelimit : 4;
  core->shared = engine_shared_state_create(queue_limit, queue_limit);
  core->state_machine = engine_state_machine_create();
  core->processing_params = processing_parameters_create(
      capture_device_config_get_channels(&config->devices.capture),
      playback_device_config_get_channels(&config->devices.playback));

  double capture_rate = (double)(config->devices.has_capture_samplerate
                                     ? config->devices.capture_samplerate
                                     : config->devices.samplerate);
  core->dop_decoder = dop_decoder_create(
      capture_device_config_get_channels(&config->devices.capture),
      capture_rate,
      capture_device_config_get_bypass_dop(&config->devices.capture),
      capture_device_config_get_dop_cutoff_hz(&config->devices.capture));

  double playback_rate = (double)config->devices.samplerate;
  sdm_filter_t dop_filter =
      playback_device_config_get_dop_encoder_filter(&config->devices.playback);
  if (dop_filter == SDM_FILTER_INVALID) {
    dop_filter = SDM_FILTER_SDM6;
  }
  core->dop_encoder = dop_encoder_create(
      playback_device_config_get_channels(&config->devices.playback),
      playback_rate,
      playback_device_config_get_output_dop(&config->devices.playback),
      dop_filter, 20000.0);

  if (!core->shared || !core->state_machine || !core->processing_params ||
      !core->dop_decoder || !core->dop_encoder) {
    core->current_config = NULL;
    dsp_engine_core_free(core);
    return NULL;
  }

  // Log configuration details and read properties to satisfy Periphery
  logger_t logger = logger_create("dsp.engine.core");
  logger_info(&logger, "Engine initialized with queueLimit: %d",
              log_arg_int((int64_t)queue_limit), log_arg_none(), log_arg_none(),
              log_arg_none());

  if (config->devices.has_rate_measure_interval) {
    logger_info(&logger,
                "Rate measure interval configured: %f s (unused on CoreAudio "
                "due to event-driven HAL listener)",
                log_arg_double(config->devices.rate_measure_interval),
                log_arg_none(), log_arg_none(), log_arg_none());
  }
  if (config->devices.has_multithreaded && config->devices.multithreaded) {
    logger_info(&logger,
                "Multithreaded processing enabled (using GCD thread pool)",
                log_arg_none(), log_arg_none(), log_arg_none(), log_arg_none());
  }
  if (config->devices.has_worker_threads) {
    logger_info(&logger,
                "Worker threads requested: %d (ignored on Apple platform as "
                "GCD automatically manages worker thread pools)",
                log_arg_int((int64_t)config->devices.worker_threads),
                log_arg_none(), log_arg_none(), log_arg_none());
  }

  return core;
}

void dsp_engine_core_free(dsp_engine_core_t* core) {
  if (!core) return;
  dsp_engine_core_stop(core,
                       (processing_stop_reason_t){.type = STOP_REASON_NONE});
  if (core->current_config) {
    dsp_config_free(core->current_config);
    core->current_config = NULL;
  }
  if (core->processing_params)
    processing_parameters_free(core->processing_params);
  if (core->state_machine) engine_state_machine_free(core->state_machine);
  if (core->shared) engine_shared_state_free(core->shared);
  if (core->dop_decoder) dop_decoder_free(core->dop_decoder);
  if (core->dop_encoder) dop_encoder_free(core->dop_encoder);
  free(core);
}

processing_state_t dsp_engine_core_get_state(const dsp_engine_core_t* core) {
  return core && core->state_machine
             ? engine_state_machine_get_state(core->state_machine)
             : PROCESSING_STATE_INACTIVE;
}

const processing_stop_reason_t* dsp_engine_core_get_stop_reason(
    const dsp_engine_core_t* core) {
  return core && core->state_machine
             ? engine_state_machine_get_stop_reason(core->state_machine)
             : NULL;
}

// MARK: - Lifecycle

/**
 * @brief Maps internal device-specific backend error codes to the public audio
 * backend error types.
 *
 * @param type The internal backend_error_type_t error type.
 * @return The mapped audio_backend_error_type_t public error type.
 */
static audio_backend_error_type_t map_backend_error(backend_error_type_t type) {
  switch (type) {
    case BACKEND_ERROR_DEVICE_NOT_FOUND:
      return AUDIO_BACKEND_ERR_DEVICE_NOT_FOUND;
    case BACKEND_ERROR_DEVICE_BUSY:
      return AUDIO_BACKEND_ERR_DEVICE_BUSY;
    default:
      return AUDIO_BACKEND_ERR_COMMAND_SEND;
  }
}

bool dsp_engine_core_start(dsp_engine_core_t* core,
                           audio_backend_error_t* err) {
  if (!core) return false;
  config_error_t cerr;
  config_error_init(&cerr);
  logger_t logger = logger_create("dsp.engine.core");
  if (dsp_engine_core_get_state(core) != PROCESSING_STATE_INACTIVE) {
    logger_warn(&logger, "Engine already running", log_arg_none(),
                log_arg_none(), log_arg_none(), log_arg_none());
    return true;
  }

  engine_state_machine_set_state(core->state_machine,
                                 PROCESSING_STATE_STARTING);
  atomic_store_explicit(&core->shared->should_stop, false,
                        memory_order_release);
  logger_info(&logger, "Starting DSP engine", log_arg_none(), log_arg_none(),
              log_arg_none(), log_arg_none());

  // Resolve capture/playback rates. `capture_samplerate` is the
  // configuration option for "capture device runs at a different rate
  // than the engine pipeline" — when set it forces the capture
  // backend to open at that rate and configures the resampler
  // with a non-1:1 base ratio. When unset both rates collapse
  // to `samplerate` and any resampler runs at 1:1 (used solely
  // as a drift-correction surface for rate-adjust).
  // 1. Resolve sampling rates.
  // The capture device can run at a different sample rate than the DSP
  // pipeline. If `capture_samplerate` is specified, we configure the resampler
  // to handle the conversion. If not, both collapse to the pipeline's rate and
  // resampler runs 1:1 (or is bypassed).
  size_t pipeline_rate = core->current_config->devices.samplerate;
  size_t capture_rate = core->current_config->devices.has_capture_samplerate
                            ? core->current_config->devices.capture_samplerate
                            : pipeline_rate;

  // 2. Create the resampler.
  // This is done before opening the capture backend because the resampler might
  // round the chunk size to a valid multiple (e.g., synchronous resampler).
  if (core->current_config->devices.has_resampler) {
    core->resampler = audio_resampler_create_from_config(
        &core->current_config->devices.resampler, capture_rate, pipeline_rate,
        capture_device_config_get_channels(
            &core->current_config->devices.capture),
        core->current_config->devices.chunksize, &cerr);
    if (!core->resampler) {
      if (err) {
        err->type = AUDIO_BACKEND_ERR_COMMAND_SEND;
        strncpy(err->message, cerr.message, sizeof(err->message) - 1);
        err->message[sizeof(err->message) - 1] = '\0';
      }
      return false;
    }
  } else {
    core->resampler = NULL;
  }

  // 3. Adopt the chunk sizes.
  // We size our capture and playback buffers based on the resampler's
  // inputs/outputs.
  size_t requested_chunk_size = core->current_config->devices.chunksize;
  size_t capture_chunk_size =
      core->resampler ? audio_resampler_get_chunk_size(core->resampler)
                      : requested_chunk_size;
  size_t playback_chunk_size =
      core->resampler ? audio_resampler_get_max_output_frames(core->resampler)
                      : capture_chunk_size;
  core->effective_playback_chunk_size = playback_chunk_size;

  if (capture_chunk_size != requested_chunk_size) {
    logger_info(&logger,
                "Adopting resampler chunkSize=%d (config requested %d)",
                log_arg_int((int64_t)capture_chunk_size),
                log_arg_int((int64_t)requested_chunk_size), log_arg_none(),
                log_arg_none());
  }

  // 4. Check for ASIO Full-Duplex.
  // If both backends are ASIO and target the exact same device, we tell the
  // backends to run in full-duplex mode to prevent opening the device driver
  // twice (which is illegal in ASIO).
  bool full_duplex = false;
#if defined(ENABLE_ASIO)
  if (core->current_config->devices.capture.type == AUDIO_BACKEND_TYPE_ASIO &&
      core->current_config->devices.playback.type == AUDIO_BACKEND_TYPE_ASIO &&
      strcmp(capture_device_config_get_device(
                 &core->current_config->devices.capture),
             playback_device_config_get_device(
                 &core->current_config->devices.playback)) == 0) {
    full_duplex = true;
  }
#endif

  // 5. Create and open capture/playback backends.
  backend_error_t berr;
  backend_error_init(&berr, BACKEND_ERROR_NONE, "");
  core->capture = create_capture_backend(
      &core->current_config->devices.capture, (int)capture_rate,
      (int)capture_chunk_size, full_duplex, core->processing_params, &berr);
  if (!core->capture || berr.type != BACKEND_ERROR_NONE) {
    if (err) {
      err->type = map_backend_error(berr.type);
      snprintf(err->message, sizeof(err->message), "%s", berr.message);
    }
    return false;
  }
  core->playback = create_playback_backend(
      &core->current_config->devices.playback, (int)pipeline_rate,
      (int)playback_chunk_size, full_duplex, core->processing_params, &berr);
  if (!core->playback || berr.type != BACKEND_ERROR_NONE) {
    if (err) {
      err->type = map_backend_error(berr.type);
      snprintf(err->message, sizeof(err->message), "%s", berr.message);
    }
    return false;
  }

  if (!capture_backend_open(core->capture, &berr)) {
    if (err) {
      err->type = map_backend_error(berr.type);
      snprintf(err->message, sizeof(err->message), "%s", berr.message);
    }
    return false;
  }
  if (!playback_backend_open(core->playback, &berr)) {
    if (err) {
      err->type = map_backend_error(berr.type);
      snprintf(err->message, sizeof(err->message), "%s", berr.message);
    }
    return false;
  }

  // 6. Prefill the playback buffer.
  // Pre-filling with silent frames ensures that the playback thread has data
  // to feed the DAC immediately on start, preventing immediate buffer underrun
  // errors. If rate adjust is enabled, we match its target level; otherwise, we
  // pre-fill 4 chunks.
  size_t prefill_frames =
      core->current_config->devices.has_target_level
          ? (size_t)core->current_config->devices.target_level
          : playback_chunk_size;
  playback_backend_prefill_silence(core->playback, prefill_frames, &berr);

  // 7. Allocate scratch chunks.
  // Allocate chunks for temporary data storage during processing/resampling.
  core->resampler_scratch = audio_chunk_create(
      core->resampler ? audio_resampler_get_max_output_frames(core->resampler)
                      : capture_chunk_size,
      capture_device_config_get_channels(
          &core->current_config->devices.capture));
  audio_chunk_set_valid_frames(core->resampler_scratch, 0);
  core->pipeline_scratch = audio_chunk_create(
      playback_chunk_size, playback_device_config_get_channels(
                               &core->current_config->devices.playback));
  audio_chunk_set_valid_frames(core->pipeline_scratch, 0);

  // 8. Create the DSP processing pipeline.
  core->pipeline =
      pipeline_create(core->current_config, core->processing_params,
                      playback_chunk_size, &cerr);
  if (!core->pipeline) {
    if (err) {
      err->type = AUDIO_BACKEND_ERR_COMMAND_SEND;
      strncpy(err->message, cerr.message, sizeof(err->message) - 1);
      err->message[sizeof(err->message) - 1] = '\0';
    }
    return false;
  }

  // 9. Pre-allocate chunk pools.
  // Allocate memory for chunk pools ahead of time to guarantee that the capture
  // and processing loop threads never perform dynamic memory allocations on the
  // hot path.
  size_t capture_pool_cap =
      spsc_queue_get_capacity(core->shared->captured_queue) + 4;
  core->capture_chunk_pool = round_robin_chunk_pool_create(
      capture_pool_cap, capture_chunk_size,
      capture_device_config_get_channels(
          &core->current_config->devices.capture));

  size_t processing_pool_cap =
      spsc_queue_get_capacity(core->shared->processed_queue) + 4;
  core->processing_scratch_pool = round_robin_chunk_pool_create(
      processing_pool_cap, playback_chunk_size,
      playback_device_config_get_channels(
          &core->current_config->devices.playback));

  if (!core->capture_chunk_pool || !core->processing_scratch_pool) {
    if (err) {
      err->type = AUDIO_BACKEND_ERR_COMMAND_SEND;
      snprintf(err->message, sizeof(err->message),
               "Failed to allocate chunk pools");
    }
    dsp_engine_core_stop(core,
                         (processing_stop_reason_t){.type = STOP_REASON_NONE});
    return false;
  }

  // 10. Instantiate the loop orchestrators.
  core->capture_loop = engine_capture_loop_create(
      core->shared, core->state_machine, core->capture, core->playback,
      core->processing_params, core->dop_decoder, core->capture_chunk_pool,
      capture_chunk_size,
      capture_device_config_get_channels(
          &core->current_config->devices.capture),
      capture_rate,
      core->current_config->devices.has_silence_threshold
          ? core->current_config->devices.silence_threshold
          : 0.0,
      core->current_config->devices.has_silence_timeout
          ? core->current_config->devices.silence_timeout
          : 0.0,
      core->current_config->devices.has_stop_on_rate_change
          ? core->current_config->devices.stop_on_rate_change
          : false,
      core->current_config->devices.has_rate_measure_interval
          ? core->current_config->devices.rate_measure_interval
          : 1.0);

  core->processing_loop = engine_processing_loop_create(
      core->shared, core->state_machine, core->processing_params, pipeline_rate,
      core->resampler, core->pipeline, core->dop_encoder,
      core->resampler_scratch, core->pipeline_scratch,
      core->processing_scratch_pool, core->on_chunk_captured,
      core->on_chunk_captured_ctx, core->on_chunk_processed,
      core->on_chunk_processed_ctx);

  bool rate_adjust_enabled =
      core->current_config->devices.has_enable_rate_adjust
          ? core->current_config->devices.enable_rate_adjust
          : false;
  double adjust_period = core->current_config->devices.has_adjust_period
                             ? core->current_config->devices.adjust_period
                             : 10.0;
  int target_level = core->current_config->devices.has_target_level
                         ? core->current_config->devices.target_level
                         : (int)playback_chunk_size;

  core->playback_loop = engine_playback_loop_create(
      core->shared, core->capture, core->playback, core->processing_params,
      pipeline_rate, playback_chunk_size, rate_adjust_enabled, adjust_period,
      target_level);

  if (!core->capture_loop || !core->processing_loop || !core->playback_loop) {
    if (err) {
      err->type = AUDIO_BACKEND_ERR_COMMAND_SEND;
      snprintf(err->message, sizeof(err->message),
               "Failed to create engine loops");
    }
    dsp_engine_core_stop(core,
                         (processing_stop_reason_t){.type = STOP_REASON_NONE});
    return false;
  }

  // MARK: - Private: thread spawn
  /// Wrap `Thread` construction so each spawn shares the same QoS,
  /// name pattern, and exit-group bookkeeping.
  int ret;
  ret = pthread_create(&core->capture_thread, NULL, capture_thread_func,
                       core->capture_loop);
  if (ret != 0) goto thread_error_0;

  ret = pthread_create(&core->processing_thread, NULL, processing_thread_func,
                       core->processing_loop);
  if (ret != 0) goto thread_error_1;

  ret = pthread_create(&core->playback_thread, NULL, playback_thread_func,
                       core->playback_loop);
  if (ret != 0) goto thread_error_2;

  core->threads_created = true;
  goto spawn_success;

thread_error_2:
  atomic_store_explicit(&core->shared->should_stop, true, memory_order_release);
  engine_sem_signal(core->shared->captured_semaphore);
  pthread_join(core->processing_thread, NULL);
thread_error_1:
  atomic_store_explicit(&core->shared->should_stop, true, memory_order_release);
  pthread_join(core->capture_thread, NULL);
thread_error_0:
  if (err) {
    err->type = AUDIO_BACKEND_ERR_COMMAND_SEND;
    snprintf(err->message, sizeof(err->message),
             "Failed to create engine threads");
  }
  dsp_engine_core_stop(core,
                       (processing_stop_reason_t){.type = STOP_REASON_NONE});
  return false;

spawn_success:

  engine_state_machine_set_state(core->state_machine, PROCESSING_STATE_RUNNING);
  logger_info(&logger, "DSP engine started: %dHz, chunk=%d",
              log_arg_int((int64_t)core->current_config->devices.samplerate),
              log_arg_int((int64_t)capture_chunk_size), log_arg_none(),
              log_arg_none());

  return true;
}

void dsp_engine_core_stop(dsp_engine_core_t* core,
                          processing_stop_reason_t reason) {
  if (!core) return;
  // Idempotent. Only the first caller drives teardown — concurrent
  // requests (typically the captureLoop's format-change report
  // racing with the actor's `previous.stop(.none)`) just return.
  if (!engine_state_machine_begin_stop(core->state_machine, reason)) return;
  if (dsp_engine_core_get_state(core) == PROCESSING_STATE_INACTIVE) return;

  logger_t logger = logger_create("dsp.engine.core");
  logger_info(&logger, "Stopping engine", log_arg_none(), log_arg_none(),
              log_arg_none(), log_arg_none());

  // Signal to the three background loops that they should exit their execution
  // loops.
  atomic_store_explicit(&core->shared->should_stop, true, memory_order_release);

  // Wake the loops out of their semaphore waits so they can
  // observe `shouldStop` and exit cleanly.
  engine_sem_signal(core->shared->captured_semaphore);
  engine_sem_signal(core->shared->processed_semaphore);

  // Wait for all audio threads to finish.
  if (core->threads_created) {
    pthread_join(core->capture_thread, NULL);
    pthread_join(core->processing_thread, NULL);
    pthread_join(core->playback_thread, NULL);
    core->threads_created = false;
  }

  // Drain any chunks left in the lock-free queues before the
  // device handles go away. Prevents stale-chunk pollution if
  // the engine is restarted with a different config.
  spsc_queue_drain(core->shared->captured_queue);
  spsc_queue_drain(core->shared->processed_queue);

  if (core->capture) {
    capture_backend_close(core->capture);
    capture_backend_free(core->capture);
    core->capture = NULL;
  }
  if (core->playback) {
    playback_backend_close(core->playback);
    playback_backend_free(core->playback);
    core->playback = NULL;
  }
  if (core->resampler) {
    audio_resampler_free(core->resampler);
    core->resampler = NULL;
  }
  if (core->resampler_scratch) {
    audio_chunk_free(core->resampler_scratch);
    core->resampler_scratch = NULL;
  }
  if (core->pipeline_scratch) {
    audio_chunk_free(core->pipeline_scratch);
    core->pipeline_scratch = NULL;
  }
  if (core->capture_loop) {
    engine_capture_loop_free(core->capture_loop);
    core->capture_loop = NULL;
  }
  if (core->processing_loop) {
    engine_processing_loop_free(core->processing_loop);
    core->processing_loop = NULL;
    core->pipeline = NULL;
  } else if (core->pipeline) {
    pipeline_free(core->pipeline);
    core->pipeline = NULL;
  }
  if (core->playback_loop) {
    engine_playback_loop_free(core->playback_loop);
    core->playback_loop = NULL;
  }

  if (core->capture_chunk_pool) {
    round_robin_chunk_pool_free(core->capture_chunk_pool);
    core->capture_chunk_pool = NULL;
  }
  if (core->processing_scratch_pool) {
    round_robin_chunk_pool_free(core->processing_scratch_pool);
    core->processing_scratch_pool = NULL;
  }

  engine_state_machine_set_state(core->state_machine,
                                 PROCESSING_STATE_INACTIVE);
  logger_info(&logger, "Engine stopped", log_arg_none(), log_arg_none(),
              log_arg_none(), log_arg_none());
}

/// Rebuild or update the processing pipeline against `newConfig` without
/// touching the audio devices. The caller is responsible for
/// verifying that `newConfig.devices == currentConfig.devices` —
/// the `DSPEngine` actor does this comparison and falls back to a
/// full teardown when they differ.
bool dsp_engine_core_reload_config(dsp_engine_core_t* core,
                                   dsp_config_t* new_config,
                                   audio_backend_error_t* err) {
  if (!core || !new_config) return false;
  logger_t logger = logger_create("dsp.engine.core");

  dsp_config_t* old_config = core->current_config;

  if (dsp_engine_core_get_state(core) == PROCESSING_STATE_INACTIVE) {
    if (old_config && old_config != new_config) {
      dsp_config_free(old_config);
    }
    core->current_config = new_config;
    return true;
  }

  // 1. Perform configuration diffing.
  // Evaluate the changes between the running config and the new config.
  config_change_t* change = config_change_create();
  if (!change) return false;
  config_change_type_t change_type =
      config_diff(old_config, new_config, change);

  if (change_type == CONFIG_CHANGE_NONE) {
    logger_info(&logger, "No changes in config.", log_arg_none(),
                log_arg_none(), log_arg_none(), log_arg_none());
    dsp_config_free(new_config);  // new config is identical, discard it
    config_change_free(change);
    return true;
  }

  // 3. Fall back to structural change (rebuilding pipeline).
  // If structural changes occurred (e.g., adding or removing filters), we must
  // rebuild the entire pipeline structure. We create the new pipeline and
  // instruct the processing loop thread to swap it. This avoids audio backend
  // restarts.
  config_change_free(change);

  config_error_t cerr;
  config_error_init(&cerr);
  pipeline_t* new_pipeline =
      pipeline_create(new_config, core->processing_params,
                      core->effective_playback_chunk_size, &cerr);
  if (!new_pipeline) {
    if (err) {
      err->type = AUDIO_BACKEND_ERR_COMMAND_SEND;
      strncpy(err->message, cerr.message, sizeof(err->message) - 1);
      err->message[sizeof(err->message) - 1] = '\0';
    }
    return false;
  }

  if (core->processing_loop) {
    engine_processing_loop_set_pipeline(core->processing_loop, new_pipeline);
  } else {
    pipeline_free(new_pipeline);
  }

  core->current_config = new_config;
  if (old_config && old_config != new_config) {
    dsp_config_free(old_config);
  }

  logger_info(&logger, "Pipeline rebuilt without audio-device restart",
              log_arg_none(), log_arg_none(), log_arg_none(), log_arg_none());
  return true;
}

void dsp_engine_core_collect_garbage(dsp_engine_core_t* core) {
  if (!core || !core->shared) return;

  if (core->shared->pipeline_garbage_queue) {
    void* p = NULL;
    while ((p = spsc_queue_dequeue(core->shared->pipeline_garbage_queue)) !=
           NULL) {
      pipeline_free((pipeline_t*)p);
    }
  }
}
