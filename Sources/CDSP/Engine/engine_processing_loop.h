#ifndef CLIB_ENGINE_ENGINE_PROCESSING_LOOP_H
#define CLIB_ENGINE_ENGINE_PROCESSING_LOOP_H

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

#include <stdbool.h>
#include <stddef.h>

#include "Audio/processing_parameters.h"
#include "Config/configuration.h"
#include "DoP/dop_encoder.h"
#include "Pipeline/pipeline.h"
#include "Resampler/audio_resampler.h"
#include "engine_shared_state.h"
#include "engine_state_machine.h"

typedef void (*chunk_callback_t)(void* ctx, const audio_chunk_t* chunk);

typedef struct {
  dsp_config_t* config;
  char** filters;
  size_t filters_count;
  char** mixers;
  size_t mixers_count;
  char** processors;
  size_t processors_count;
} pending_update_t;

/// `@unchecked Sendable` is a *transfer* vouch, not a *share*
/// vouch: the instance is safe to cross the Thread spawn boundary
/// because exactly one thread (the loop thread) ever touches it
/// after `run()` is invoked. The scratch chunks have no internal
/// synchronisation and are *not* safe to use from multiple threads
/// concurrently.
typedef struct {
  engine_shared_state_t* shared;
  engine_state_machine_t* state_machine;
  processing_parameters_t* processing_params;
  size_t pipeline_rate;
  audio_resampler_t* resampler;
  pipeline_t* active_pipeline;
  dop_encoder_t* dop_encoder;
  spsc_queue_t* pipeline_queue;
  spsc_queue_t* update_queue;
  audio_chunk_t* resampler_scratch;
  audio_chunk_t* pipeline_scratch;

  chunk_callback_t on_chunk_captured;
  void* on_chunk_captured_ctx;
  chunk_callback_t on_chunk_processed;
  void* on_chunk_processed_ctx;

  engine_stop_callback_t on_stop;
  void* on_stop_ctx;
} engine_processing_loop_t;

engine_processing_loop_t* engine_processing_loop_create(
    engine_shared_state_t* shared, engine_state_machine_t* state_machine,
    processing_parameters_t* processing_params, size_t pipeline_rate,
    audio_resampler_t* resampler, pipeline_t* pipeline,
    dop_encoder_t* dop_encoder, audio_chunk_t* resampler_scratch,
    audio_chunk_t* pipeline_scratch, chunk_callback_t on_chunk_captured,
    void* on_chunk_captured_ctx, chunk_callback_t on_chunk_processed,
    void* on_chunk_processed_ctx, engine_stop_callback_t on_stop,
    void* on_stop_ctx);

void engine_processing_loop_free(engine_processing_loop_t* loop);
void engine_processing_loop_run(engine_processing_loop_t* loop);
void engine_processing_loop_set_pipeline(engine_processing_loop_t* loop,
                                         pipeline_t* new_pipeline);
void engine_processing_loop_enqueue_update(engine_processing_loop_t* loop,
                                           pending_update_t* update);
void pending_update_free(pending_update_t* update);

#endif  // CLIB_ENGINE_ENGINE_PROCESSING_LOOP_H
