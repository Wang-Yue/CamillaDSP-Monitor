#ifndef CLIB_ENGINE_ENGINE_PROCESSING_LOOP_H
#define CLIB_ENGINE_ENGINE_PROCESSING_LOOP_H

/**
 * @file engine_processing_loop.h
 * @brief Processing thread loop for the DSP engine.
 *
 * Drains the capture→processing SPSC queue, runs each chunk through the
 * (optional) resampler and the pipeline, then enqueues the result on the
 * processing→playback queue.
 *
 * @section state_ownership State ownership
 * The pre-allocated scratch chunks (`resamplerScratch`, `pipelineScratch`) are
 * owned by this loop and only mutated here. The resampler's own internal state
 * is also single-threaded: the playback thread publishes a relative ratio via
 * the shared atomic, and the processing thread consumes it once per chunk
 * through `setRelativeRatio`. No cross-thread mutation of resampler state.
 *
 * @section audio_invariants Audio-thread invariants
 * - No allocations in the steady state. Output chunks are obtained from a
 * pre-allocated `RoundRobinChunkPool`, and the resampler scratch chunk is
 * pre-allocated at init.
 * - No locks. The shared SPSC queues + semaphores carry chunks and wakeups; the
 * resampler ratio is an atomic Double.
 * - The thread sets a real-time scheduling policy on entry so the OS prefers it
 * over background work.
 */

#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Audio/processing_parameters.h"
#include "Config/configuration.h"
#include "DoP/dop_encoder.h"
#include "Pipeline/pipeline.h"
#include "Resampler/audio_resampler.h"
#include "engine_shared_state.h"
#include "engine_state_machine.h"

/**
 * @brief Callback function type for audio chunk events.
 *
 * @param ctx User-defined context pointer passed to the callback.
 * @param chunk Pointer to the audio chunk.
 */
typedef void (*chunk_callback_t)(void* ctx, const audio_chunk_t* chunk);

/**
 * @brief Opaque structure representing the processing loop.
 *
 * `@unchecked Sendable` is a *transfer* vouch, not a *share*
 * vouch: the instance is safe to cross the Thread spawn boundary
 * because exactly one thread (the loop thread) ever touches it
 * after `run()` is invoked. The scratch chunks have no internal
 * synchronisation and are *not* safe to use from multiple threads
 * concurrently.
 */
typedef struct engine_processing_loop engine_processing_loop_t;

/**
 * @brief Creates a new engine processing loop instance.
 *
 * @param shared Pointer to the shared state.
 * @param state_machine Pointer to the engine state machine.
 * @param processing_params Pointer to the processing parameters.
 * @param pipeline_rate Rate of the pipeline.
 * @param resampler Pointer to the audio resampler (optional, can be NULL).
 * @param pipeline Pointer to the processing pipeline.
 * @param dop_encoder Pointer to the DoP encoder (optional, can be NULL).
 * @param resampler_scratch Pointer to the resampler scratch buffer.
 * @param pipeline_scratch Pointer to the pipeline scratch buffer.
 * @param scratch_pool Pointer to the chunk pool.
 * @param on_chunk_captured Callback called when a chunk is captured.
 * @param on_chunk_captured_ctx Context pointer for the captured callback.
 * @param on_chunk_processed Callback called when a chunk is processed.
 * @param on_chunk_processed_ctx Context pointer for the processed callback.
 * @return Pointer to the created processing loop instance, or NULL on failure.
 */
engine_processing_loop_t* engine_processing_loop_create(
    engine_shared_state_t* shared, engine_state_machine_t* state_machine,
    processing_parameters_t* processing_params, size_t pipeline_rate,
    audio_resampler_t* resampler, pipeline_t* pipeline,
    dop_encoder_t* dop_encoder, audio_chunk_t* resampler_scratch,
    audio_chunk_t* pipeline_scratch, round_robin_chunk_pool_t* scratch_pool,
    chunk_callback_t on_chunk_captured, void* on_chunk_captured_ctx,
    chunk_callback_t on_chunk_processed, void* on_chunk_processed_ctx);

/**
 * @brief Frees the engine processing loop instance.
 *
 * @param loop Pointer to the processing loop instance to free.
 */
void engine_processing_loop_free(engine_processing_loop_t* loop);

/**
 * @brief Runs the processing loop.
 *
 * This function blocks and runs the processing loop until it is requested to
 * stop or an error occurs.
 *
 * @param loop Pointer to the processing loop instance.
 */
void engine_processing_loop_run(engine_processing_loop_t* loop);

/**
 * @brief Sets a new pipeline for the processing loop.
 *
 * @param loop Pointer to the processing loop instance.
 * @param new_pipeline Pointer to the new pipeline.
 */
void engine_processing_loop_set_pipeline(engine_processing_loop_t* loop,
                                         pipeline_t* new_pipeline);

#endif  // CLIB_ENGINE_ENGINE_PROCESSING_LOOP_H
