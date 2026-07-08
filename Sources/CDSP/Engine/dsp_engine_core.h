/**
 * @file dsp_engine_core.h
 * @brief Top-level engine orchestrator.
 *
 * This class owns the *shape* of an engine run — config, sizing,
 * device handles, the three audio threads — but contains no audio
 * processing logic itself. Each thread body lives in its own file:
 *
 *   - `EngineCaptureLoop`     - capture -> DoP-decode -> level meter -> SPSC
 * queue.
 *   - `EngineProcessingLoop`  - SPSC dequeue -> resample -> pipeline -> SPSC
 * enqueue.
 *   - `EnginePlaybackLoop`    - SPSC dequeue -> rate-adjust controller ->
 * device write.
 *
 * All cross-thread state (the stop flag, the SPSC queues, the
 * resampler-ratio atomic) lives in `EngineSharedState`. State
 * machine + stop-reason publication lives in `EngineStateMachine`.
 *
 * Lock-free / allocation-free guarantees:
 * - The audio threads use lock-free SPSC queues and atomics;
 *   only `DispatchSemaphore` is used for signal/wait, which is
 *   a kernel signaling primitive (not a lock).
 * - Chunks are managed using a pre-allocated `RoundRobinChunkPool`
 *   on each thread to avoid allocations on the hot path.
 * - The resampler and pipeline output scratch buffers are pre-allocated.
 * - The stall watchdog uses `clock_gettime_nsec_np` (vDSO read,
 *   no syscall) — no `Date()` on the hot path.
 */

#ifndef CLIB_ENGINE_DSP_ENGINE_CORE_H
#define CLIB_ENGINE_DSP_ENGINE_CORE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Audio/processing_parameters.h"
#include "Backend/audio_backend.h"
#include "Config/configuration.h"
#include "engine_capture_loop.h"
#include "engine_playback_loop.h"
#include "engine_processing_loop.h"
#include "engine_shared_state.h"
#include "engine_state_machine.h"

/**
 * @brief Core structure of the DSP engine.
 *
 * Orchestrates the audio threads, backends, and processing pipeline.
 */
typedef struct {
  // MARK: - Configuration
  /** Current configuration. */
  dsp_config_t* current_config;
  /** Processing parameters derived from configuration. */
  processing_parameters_t* processing_params;

  // MARK: - Shared state
  /** State machine managing engine state. */
  engine_state_machine_t* state_machine;
  /** Shared state between threads. */
  engine_shared_state_t* shared;

  // MARK: - Components built per run
  /** Capture audio backend. */
  capture_backend_t* capture;
  /** Playback audio backend. */
  playback_backend_t* playback;
  /** Processing loop instance. */
  engine_processing_loop_t* processing_loop;
  /** Capture loop instance. */
  engine_capture_loop_t* capture_loop;
  /** Playback loop instance. */
  engine_playback_loop_t* playback_loop;
  /** DoP decoder instance. */
  dop_decoder_t* dop_decoder;
  /** DoP encoder instance. */
  dop_encoder_t* dop_encoder;

  /**
   * Playback-side chunk size — `resampler.maxOutputFrames` when a
   * resampler is in use, otherwise `effectiveChunkSize`.
   */
  size_t effective_playback_chunk_size;
  /** Scratch buffer for resampler. */
  audio_chunk_t* resampler_scratch;
  /** Scratch buffer for pipeline. */
  audio_chunk_t* pipeline_scratch;
  /** Pool of chunks for capture. */
  round_robin_chunk_pool_t* capture_chunk_pool;
  /** Pool of scratch chunks for processing. */
  round_robin_chunk_pool_t* processing_scratch_pool;
  /** Audio resampler. */
  audio_resampler_t* resampler;
  /** Audio processing pipeline. */
  pipeline_t* pipeline;

  // MARK: - Threading
  /** Capture thread handle. */
  pthread_t capture_thread;
  /** Processing thread handle. */
  pthread_t processing_thread;
  /** Playback thread handle. */
  pthread_t playback_thread;
  /** True if threads were successfully created. */
  bool threads_created;

  // MARK: - Optional taps for visualisation
  /**
   * Optional callback invoked before pipeline processing.
   * Set before `start()` and treated as immutable thereafter.
   */
  chunk_callback_t on_chunk_captured;
  /** Context for capture callback. */
  void* on_chunk_captured_ctx;

  /**
   * Optional callback invoked after pipeline processing.
   * Set before `start()` and treated as immutable thereafter.
   */
  chunk_callback_t on_chunk_processed;
  /** Context for processing callback. */
  void* on_chunk_processed_ctx;
} dsp_engine_core_t;

/**
 * @brief Create a new DSP engine core.
 *
 * @param config Configuration to initialize the core with.
 * @return Pointer to the created core, or NULL on failure.
 */
dsp_engine_core_t* dsp_engine_core_create(dsp_config_t* config);

/**
 * @brief Free the DSP engine core.
 * @param core Pointer to the core instance.
 */
void dsp_engine_core_free(dsp_engine_core_t* core);

/**
 * @brief Get the current processing state.
 *
 * @param core Pointer to the core instance.
 * @return Current state.
 */
processing_state_t dsp_engine_core_get_state(const dsp_engine_core_t* core);

/**
 * @brief Get the reason why processing stopped.
 *
 * @param core Pointer to the core instance.
 * @return Pointer to the stop reason, or NULL if not stopped.
 */
const processing_stop_reason_t* dsp_engine_core_get_stop_reason(
    const dsp_engine_core_t* core);

/**
 * @brief Start the DSP engine processing threads.
 *
 * @param core Pointer to the core instance.
 * @param err Pointer to store backend errors if starting fails.
 * @return True on success, false on failure.
 */
bool dsp_engine_core_start(dsp_engine_core_t* core, audio_backend_error_t* err);

/**
 * @brief Stop the DSP engine processing threads.
 *
 * @param core Pointer to the core instance.
 * @param reason Reason for stopping.
 */
void dsp_engine_core_stop(dsp_engine_core_t* core,
                          processing_stop_reason_t reason);

/**
 * @brief Reload the configuration.
 *
 * Rebuilds or updates the processing pipeline against `newConfig` without
 * touching the audio devices. The caller is responsible for verifying that
 * device configurations match.
 *
 * @param core Pointer to the core instance.
 * @param new_config Pointer to the new configuration.
 * @param err Pointer to store backend errors if reload fails.
 * @return True on success, false on failure.
 */
bool dsp_engine_core_reload_config(dsp_engine_core_t* core,
                                   dsp_config_t* new_config,
                                   audio_backend_error_t* err);

/**
 * @brief Dequeues and frees garbage generated by the audio thread.
 *
 * Runs on the control plane.
 *
 * @param core Pointer to the core instance.
 */
void dsp_engine_core_collect_garbage(dsp_engine_core_t* core);

#endif  // CLIB_ENGINE_DSP_ENGINE_CORE_H
