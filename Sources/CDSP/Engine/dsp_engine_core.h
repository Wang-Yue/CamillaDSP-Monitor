#ifndef CLIB_ENGINE_DSP_ENGINE_CORE_H
#define CLIB_ENGINE_DSP_ENGINE_CORE_H

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

#include "engine_shared_state.h"
#include "engine_state_machine.h"
#include "engine_capture_loop.h"
#include "engine_playback_loop.h"
#include "engine_processing_loop.h"
#include "Config/configuration.h"
#include "Audio/processing_parameters.h"
#include "Backend/audio_backend.h"
#include <pthread.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // MARK: - Configuration
    dsp_config_t* current_config;
    processing_parameters_t* processing_params;

    // MARK: - Shared state
    engine_state_machine_t* state_machine;
    engine_shared_state_t* shared;

    // MARK: - Components built per run
    capture_backend_t* capture;
    playback_backend_t* playback;
    engine_processing_loop_t* processing_loop;
    engine_capture_loop_t* capture_loop;
    engine_playback_loop_t* playback_loop;
    dop_decoder_t* dop_decoder;
    dop_encoder_t* dop_encoder;

    /// Playback-side chunk size — `resampler.maxOutputFrames` when a
    /// resampler is in use, otherwise `effectiveChunkSize`.
    size_t effective_playback_chunk_size;
    audio_chunk_t* resampler_scratch;
    audio_chunk_t* pipeline_scratch;
    audio_resampler_t* resampler;
    pipeline_t* pipeline;

    // MARK: - Threading
    pthread_t capture_thread;
    pthread_t processing_thread;
    pthread_t playback_thread;
    bool threads_created;

    // MARK: - Optional taps for visualisation
    /// Optional callback invoked before pipeline processing on the
    /// processing thread. Set before `start()` and treated as
    /// immutable thereafter.
    chunk_callback_t on_chunk_captured;
    void* on_chunk_captured_ctx;

    /// Optional callback invoked after pipeline processing on the
    /// processing thread. Set before `start()` and treated as
    /// immutable thereafter.
    chunk_callback_t on_chunk_processed;
    void* on_chunk_processed_ctx;
} dsp_engine_core_t;

dsp_engine_core_t* dsp_engine_core_create(dsp_config_t* config);
void dsp_engine_core_free(dsp_engine_core_t* core);

processing_state_t dsp_engine_core_get_state(const dsp_engine_core_t* core);
const processing_stop_reason_t* dsp_engine_core_get_stop_reason(const dsp_engine_core_t* core);

bool dsp_engine_core_start(dsp_engine_core_t* core, audio_backend_error_t* err);
void dsp_engine_core_stop(dsp_engine_core_t* core, processing_stop_reason_t reason);

/// Rebuild or update the processing pipeline against `newConfig` without
/// touching the audio devices. The caller is responsible for
/// verifying that `newConfig.devices == currentConfig.devices` —
/// the `DSPEngine` actor does this comparison and falls back to a
/// full teardown when they differ.
bool dsp_engine_core_reload_config(dsp_engine_core_t* core, dsp_config_t* new_config, audio_backend_error_t* err);

#ifdef __cplusplus
}
#endif

#endif // CLIB_ENGINE_DSP_ENGINE_CORE_H
