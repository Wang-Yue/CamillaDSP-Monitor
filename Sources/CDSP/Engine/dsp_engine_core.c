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
#include "Logging/app_logger.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void* capture_thread_func(void* arg) {
    engine_capture_loop_t* loop = (engine_capture_loop_t*)arg;
    engine_capture_loop_run(loop);
    return NULL;
}

static void* processing_thread_func(void* arg) {
    engine_processing_loop_t* loop = (engine_processing_loop_t*)arg;
    engine_processing_loop_run(loop);
    return NULL;
}

static void* playback_thread_func(void* arg) {
    engine_playback_loop_t* loop = (engine_playback_loop_t*)arg;
    engine_playback_loop_run(loop);
    return NULL;
}

static void core_on_stop_callback(void* ctx, processing_stop_reason_t reason) {
    dsp_engine_core_t* core = (dsp_engine_core_t*)ctx;
    if (core) {
        dsp_engine_core_stop(core, reason);
    }
}

// MARK: - Init

dsp_engine_core_t* dsp_engine_core_create(dsp_config_t* config) {
    if (!config) return NULL;
    dsp_engine_core_t* core = (dsp_engine_core_t*)calloc(1, sizeof(dsp_engine_core_t));
    if (!core) return NULL;

    core->current_config = config;
    int queue_limit = config->devices.has_queuelimit ? config->devices.queuelimit : 4;
    core->shared = engine_shared_state_create(queue_limit, queue_limit);
    core->state_machine = engine_state_machine_create();
    core->processing_params = processing_parameters_create(config->devices.capture.channels, config->devices.playback.channels);

    double capture_rate = (double)(config->devices.has_capture_samplerate ? config->devices.capture_samplerate : config->devices.samplerate);
    core->dop_decoder = dop_decoder_create(config->devices.capture.channels, capture_rate, config->devices.capture.has_bypass_dop ? config->devices.capture.bypass_dop : false, config->devices.capture.has_dop_cutoff_hz ? config->devices.capture.dop_cutoff_hz : 20000.0);

    double playback_rate = (double)config->devices.samplerate;
    sdm_filter_t dop_filter = config->devices.playback.has_dop_encoder_filter ? config->devices.playback.dop_encoder_filter : SDM_FILTER_SDM6;
    core->dop_encoder = dop_encoder_create(config->devices.playback.channels, playback_rate, config->devices.playback.has_output_dop ? config->devices.playback.output_dop : false, dop_filter, 20000.0);

    // Log configuration details and read properties to satisfy Periphery
    logger_t logger = logger_create("dsp.engine.core");
    logger_info(&logger, "Engine initialized with queueLimit: %d", log_arg_int((int64_t)queue_limit), log_arg_none(), log_arg_none(), log_arg_none());

    return core;
}

void dsp_engine_core_free(dsp_engine_core_t* core) {
    if (!core) return;
    dsp_engine_core_stop(core, (processing_stop_reason_t){ .type = STOP_REASON_NONE });
    if (core->current_config) {
        dsp_config_free(core->current_config);
        core->current_config = NULL;
    }
    if (core->processing_params) processing_parameters_free(core->processing_params);
    if (core->state_machine) engine_state_machine_free(core->state_machine);
    if (core->shared) engine_shared_state_free(core->shared);
    if (core->dop_decoder) dop_decoder_free(core->dop_decoder);
    if (core->dop_encoder) dop_encoder_free(core->dop_encoder);
    free(core);
}

processing_state_t dsp_engine_core_get_state(const dsp_engine_core_t* core) {
    return core && core->state_machine ? engine_state_machine_get_state(core->state_machine) : PROCESSING_STATE_INACTIVE;
}

const processing_stop_reason_t* dsp_engine_core_get_stop_reason(const dsp_engine_core_t* core) {
    return core && core->state_machine ? engine_state_machine_get_stop_reason(core->state_machine) : NULL;
}

// MARK: - Lifecycle

// MARK: - Private: runtime construction
/// Bag of components built in `start()` and handed to each loop.
/// Bundling them avoids passing eight parameters around and keeps
/// the loop initialisers concise.
bool dsp_engine_core_start(dsp_engine_core_t* core, audio_backend_error_t* err) {
    if (!core) return false;
    logger_t logger = logger_create("dsp.engine.core");
    if (dsp_engine_core_get_state(core) != PROCESSING_STATE_INACTIVE) {
        logger_warn(&logger, "Engine already running", log_arg_none(), log_arg_none(), log_arg_none(), log_arg_none());
        return true;
    }

    engine_state_machine_set_state(core->state_machine, PROCESSING_STATE_STARTING);
    atomic_store_explicit(&core->shared->should_stop, false, memory_order_release);
    logger_info(&logger, "Starting DSP engine", log_arg_none(), log_arg_none(), log_arg_none(), log_arg_none());

    // Resolve capture/playback rates. `capture_samplerate` is the
    // configuration option for "capture device runs at a different rate
    // than the engine pipeline" — when set it forces the capture
    // backend to open at that rate and configures the resampler
    // with a non-1:1 base ratio. When unset both rates collapse
    // to `samplerate` and any resampler runs at 1:1 (used solely
    // as a drift-correction surface for rate-adjust).
    size_t pipeline_rate = (size_t)core->current_config->devices.samplerate;
    size_t capture_rate = (size_t)(core->current_config->devices.has_capture_samplerate ? core->current_config->devices.capture_samplerate : pipeline_rate);

    // Create the resampler first so we can adopt its (possibly
    // rounded) chunk size before opening the audio devices.
    if (core->current_config->devices.has_resampler) {
        core->resampler = audio_resampler_create_from_config(&core->current_config->devices.resampler, capture_rate, pipeline_rate, core->current_config->devices.capture.channels, core->current_config->devices.chunksize);
    } else {
        core->resampler = NULL;
    }

    // Adopt the resampler's input chunk size. `SynchronousResampler`
    // rounds the requested size up to the smallest valid multiple
    // of `inputRate / gcd(in, out)`; the rest of the engine has to
    // honour that rounded value or `process(input:into:)` will
    // throw `inputSizeMismatch`. The async resamplers don't round,
    // so this is a no-op for them.
    size_t requested_chunk_size = (size_t)core->current_config->devices.chunksize;
    size_t capture_chunk_size = core->resampler ? audio_resampler_get_chunk_size(core->resampler) : requested_chunk_size;
    size_t playback_chunk_size = core->resampler ? audio_resampler_get_max_output_frames(core->resampler) : capture_chunk_size;
    core->effective_playback_chunk_size = playback_chunk_size;

    if (capture_chunk_size != requested_chunk_size) {
        logger_info(&logger, "Adopting resampler chunkSize=%d (config requested %d)", log_arg_int((int64_t)capture_chunk_size), log_arg_int((int64_t)requested_chunk_size), log_arg_none(), log_arg_none());
    }

    backend_error_t berr;
    backend_error_init(&berr, BACKEND_ERROR_NONE, "");
    core->capture = create_capture_backend(&core->current_config->devices.capture, (int)capture_rate, (int)capture_chunk_size, &berr);
    if (!core->capture || berr.type != BACKEND_ERROR_NONE) {
        if (err) { err->type = AUDIO_BACKEND_ERR_COMMAND_SEND; snprintf(err->message, sizeof(err->message), "%s", berr.message); }
        return false;
    }
    core->playback = create_playback_backend(&core->current_config->devices.playback, (int)pipeline_rate, (int)playback_chunk_size, &berr);
    if (!core->playback || berr.type != BACKEND_ERROR_NONE) {
        if (err) { err->type = AUDIO_BACKEND_ERR_COMMAND_SEND; snprintf(err->message, sizeof(err->message), "%s", berr.message); }
        return false;
    }

    if (!capture_backend_open(core->capture, &berr)) {
        if (err) { err->type = AUDIO_BACKEND_ERR_COMMAND_SEND; snprintf(err->message, sizeof(err->message), "%s", berr.message); }
        return false;
    }
    if (!playback_backend_open(core->playback, &berr)) {
        if (err) { err->type = AUDIO_BACKEND_ERR_COMMAND_SEND; snprintf(err->message, sizeof(err->message), "%s", berr.message); }
        return false;
    }

    // Pre-fill the playback ring with zeros so the CoreAudio render
    // thread has a buffer of silence to drain during startup. If
    // rate adjust is enabled we match its target level; otherwise
    // we pre-fill a safe 4-chunk headroom.
    size_t prefill_frames = (core->current_config->devices.has_enable_rate_adjust && core->current_config->devices.enable_rate_adjust && core->current_config->devices.has_target_level) ? (size_t)core->current_config->devices.target_level : (playback_chunk_size * 2);
    if (!core->current_config->devices.has_enable_rate_adjust || !core->current_config->devices.enable_rate_adjust) {
        prefill_frames = playback_chunk_size * 4;
    }
    playback_backend_prefill_silence(core->playback, prefill_frames, &berr);

    // Pre-allocate scratch buffers sized for the worst case across
    // the configured rate-adjust range.
    core->resampler_scratch = audio_chunk_create(core->resampler ? audio_resampler_get_max_output_frames(core->resampler) : capture_chunk_size, core->current_config->devices.capture.channels);
    core->resampler_scratch->valid_frames = 0;
    core->pipeline_scratch = audio_chunk_create(playback_chunk_size, core->current_config->devices.playback.channels);
    core->pipeline_scratch->valid_frames = 0;

    config_error_t cerr;
    memset(&cerr, 0, sizeof(cerr));
    core->pipeline = pipeline_create(core->current_config, core->processing_params, playback_chunk_size, &cerr);
    if (!core->pipeline) {
        if (err) { err->type = AUDIO_BACKEND_ERR_COMMAND_SEND; snprintf(err->message, sizeof(err->message), "%s", cerr.message); }
        return false;
    }

    core->capture_loop = engine_capture_loop_create(
        core->shared, core->state_machine, core->capture, core->playback, core->processing_params,
        core->dop_decoder, capture_chunk_size, core->current_config->devices.capture.channels, capture_rate,
        core->current_config->devices.has_silence_threshold ? core->current_config->devices.silence_threshold : 0.0,
        core->current_config->devices.has_silence_timeout ? core->current_config->devices.silence_timeout : 0.0,
        core_on_stop_callback, core
    );

    core->processing_loop = engine_processing_loop_create(
        core->shared, core->state_machine, core->processing_params, pipeline_rate,
        core->resampler, core->pipeline, core->dop_encoder, core->resampler_scratch, core->pipeline_scratch,
        core->on_chunk_captured, core->on_chunk_captured_ctx,
        core->on_chunk_processed, core->on_chunk_processed_ctx,
        core_on_stop_callback, core
    );

    bool rate_adjust_enabled = core->current_config->devices.has_enable_rate_adjust ? core->current_config->devices.enable_rate_adjust : false;
    double adjust_period = core->current_config->devices.has_adjust_period ? core->current_config->devices.adjust_period : 10.0;
    int target_level = core->current_config->devices.has_target_level ? core->current_config->devices.target_level : (int)(playback_chunk_size * 2);

    core->playback_loop = engine_playback_loop_create(
        core->shared, core->capture, core->playback, core->processing_params, pipeline_rate, playback_chunk_size,
        rate_adjust_enabled, adjust_period, target_level,
        core_on_stop_callback, core
    );

    if (!core->capture_loop || !core->processing_loop || !core->playback_loop) {
        if (err) { err->type = AUDIO_BACKEND_ERR_COMMAND_SEND; snprintf(err->message, sizeof(err->message), "Failed to create engine loops"); }
        dsp_engine_core_stop(core, (processing_stop_reason_t){ .type = STOP_REASON_NONE });
        return false;
    }

    // MARK: - Private: thread spawn
    /// Wrap `Thread` construction so each spawn shares the same QoS,
    /// name pattern, and exit-group bookkeeping.
    pthread_create(&core->capture_thread, NULL, capture_thread_func, core->capture_loop);
    pthread_create(&core->processing_thread, NULL, processing_thread_func, core->processing_loop);
    pthread_create(&core->playback_thread, NULL, playback_thread_func, core->playback_loop);
    core->threads_created = true;

    engine_state_machine_set_state(core->state_machine, PROCESSING_STATE_RUNNING);
    logger_info(&logger, "DSP engine started: %dHz, chunk=%d", log_arg_int((int64_t)core->current_config->devices.samplerate), log_arg_int((int64_t)capture_chunk_size), log_arg_none(), log_arg_none());

    return true;
}

void dsp_engine_core_stop(dsp_engine_core_t* core, processing_stop_reason_t reason) {
    if (!core) return;
    // Idempotent. Only the first caller drives teardown — concurrent
    // requests (typically the captureLoop's format-change report
    // racing with the actor's `previous.stop(.none)`) just return.
    if (!engine_state_machine_begin_stop(core->state_machine, reason)) return;
    if (dsp_engine_core_get_state(core) == PROCESSING_STATE_INACTIVE) return;

    logger_t logger = logger_create("dsp.engine.core");
    logger_info(&logger, "Stopping engine", log_arg_none(), log_arg_none(), log_arg_none(), log_arg_none());
    atomic_store_explicit(&core->shared->should_stop, true, memory_order_release);

    // Wake the loops out of their semaphore waits so they can
    // observe `shouldStop` and exit cleanly.
    dispatch_semaphore_signal(core->shared->captured_semaphore);
    dispatch_semaphore_signal(core->shared->processed_semaphore);

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

    if (core->capture) { capture_backend_close(core->capture); capture_backend_free(core->capture); core->capture = NULL; }
    if (core->playback) { playback_backend_close(core->playback); playback_backend_free(core->playback); core->playback = NULL; }
    if (core->resampler) { audio_resampler_free(core->resampler); core->resampler = NULL; }
    if (core->resampler_scratch) { audio_chunk_free(core->resampler_scratch); core->resampler_scratch = NULL; }
    if (core->pipeline_scratch) { audio_chunk_free(core->pipeline_scratch); core->pipeline_scratch = NULL; }
    if (core->capture_loop) { engine_capture_loop_free(core->capture_loop); core->capture_loop = NULL; }
    if (core->processing_loop) { engine_processing_loop_free(core->processing_loop); core->processing_loop = NULL; core->pipeline = NULL; }
    else if (core->pipeline) { pipeline_free(core->pipeline); core->pipeline = NULL; }
    if (core->playback_loop) { engine_playback_loop_free(core->playback_loop); core->playback_loop = NULL; }

    engine_state_machine_set_state(core->state_machine, PROCESSING_STATE_INACTIVE);
    logger_info(&logger, "Engine stopped", log_arg_none(), log_arg_none(), log_arg_none(), log_arg_none());
}

/// Rebuild or update the processing pipeline against `newConfig` without
/// touching the audio devices. The caller is responsible for
/// verifying that `newConfig.devices == currentConfig.devices` —
/// the `DSPEngine` actor does this comparison and falls back to a
/// full teardown when they differ.
bool dsp_engine_core_reload_config(dsp_engine_core_t* core, dsp_config_t* new_config, audio_backend_error_t* err) {
    if (!core || !new_config) return false;
    logger_t logger = logger_create("dsp.engine.core");
    if (core->current_config && core->current_config != new_config) {
        dsp_config_free(core->current_config);
    }
    core->current_config = new_config;

    double capture_rate = (double)(new_config->devices.has_capture_samplerate ? new_config->devices.capture_samplerate : new_config->devices.samplerate);
    if (core->dop_decoder) dop_decoder_free(core->dop_decoder);
    core->dop_decoder = dop_decoder_create(new_config->devices.capture.channels, capture_rate, new_config->devices.capture.has_bypass_dop ? new_config->devices.capture.bypass_dop : false, new_config->devices.capture.has_dop_cutoff_hz ? new_config->devices.capture.dop_cutoff_hz : 20000.0);

    double playback_rate = (double)new_config->devices.samplerate;
    if (core->dop_encoder) dop_encoder_free(core->dop_encoder);
    sdm_filter_t dop_filter = new_config->devices.playback.has_dop_encoder_filter ? new_config->devices.playback.dop_encoder_filter : SDM_FILTER_SDM6;
    core->dop_encoder = dop_encoder_create(new_config->devices.playback.channels, playback_rate, new_config->devices.playback.has_output_dop ? new_config->devices.playback.output_dop : false, dop_filter, 20000.0);

    if (dsp_engine_core_get_state(core) == PROCESSING_STATE_INACTIVE) return true;

    // Check if we can do an in-place parameter update instead of rebuilding the pipeline.
    // Force rebuild if any mixer channel count changed (since they are immutable in Swift AudioMixer)
    config_error_t cerr;
    memset(&cerr, 0, sizeof(cerr));
    pipeline_t* new_pipeline = pipeline_create(new_config, core->processing_params, core->effective_playback_chunk_size, &cerr);
    if (!new_pipeline) {
        if (err) { err->type = AUDIO_BACKEND_ERR_COMMAND_SEND; snprintf(err->message, sizeof(err->message), "%s", cerr.message); }
        return false;
    }
    if (core->processing_loop) {
        engine_processing_loop_set_pipeline(core->processing_loop, new_pipeline);
    } else {
        pipeline_free(new_pipeline);
    }
    logger_info(&logger, "Pipeline rebuilt without audio-device restart", log_arg_none(), log_arg_none(), log_arg_none(), log_arg_none());
    return true;
}
