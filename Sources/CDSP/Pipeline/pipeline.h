#ifndef CLIB_PIPELINE_PIPELINE_H
#define CLIB_PIPELINE_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_buffers.h"
#include "Audio/audio_chunk.h"
#include "Audio/double_helpers.h"
#include "Audio/processing_parameters.h"
#include "Config/config_error.h"
#include "Config/configuration.h"
#include "Filters/filter.h"
#include "Filters/volume.h"
#include "Mixer/mixer.h"
#include "Processors/processor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  PIPELINE_OK = 0,
  PIPELINE_ERR_INPUT_SIZE_MISMATCH = -1,
  PIPELINE_ERR_OUTPUT_BUFFER_TOO_SMALL = -2,
  PIPELINE_ERR_CHANNEL_COUNT_MISMATCH = -3
} pipeline_error_t;

/// A single step in the processing pipeline
typedef enum {
  /// Filter chain applied to a single channel
  EXEC_STEP_FILTER = 0,
  /// Mixer that changes channel routing.
  EXEC_STEP_MIXER,
  /// Audio processor applied to the chunk in-place.
  EXEC_STEP_PROCESSOR
} exec_step_type_t;

/// A single step in the processing pipeline
typedef struct {
  exec_step_type_t type;
  bool bypassed;
  // For EXEC_STEP_FILTER:
  int channel;
  filter_t** filters;
  size_t filters_count;
  // For EXEC_STEP_MIXER:
  audio_mixer_t* mixer;
  // For EXEC_STEP_PROCESSOR:
  dsp_processor_t* processor;
} pipeline_exec_step_t;

/// The main audio processing pipeline.
typedef struct {
  pipeline_exec_step_t* steps;
  size_t steps_count;
  /// Implicit main volume filter with smooth ramping
  volume_filter_t* master_volume;
  /// Working scratch the pipeline copies the caller's input into at the start
  /// of each `process(...)`. With class-owned `AudioBuffers`, we can no
  /// longer rely on CoW to isolate mutations from the caller's `input`
  /// chunk — so we copy explicitly into this pre-allocated buffer.
  audio_chunk_t* capture_scratch;
  /// Pre-allocated scratch chunks mapped by the sequential step index in
  /// `steps` array to prevent Copy-On-Write allocations on the hot path.
  audio_chunk_t** scratches_for_mixers;
  size_t scratches_for_mixers_count;

  size_t frames_per_chunk;
  int rate;
  size_t expected_in_channels;
  size_t expected_out_channels;

  // For test inspection on error:
  size_t last_error_needed;
  size_t last_error_got;
} pipeline_t;

/// Initialize the main audio processing pipeline.
pipeline_t* pipeline_create(const dsp_config_t* config,
                            processing_parameters_t* proc_params,
                            size_t explicit_chunk_size, config_error_t* err);

/// Process an input audio chunk into an output audio chunk.
pipeline_error_t pipeline_process(pipeline_t* pipeline,
                                  const audio_chunk_t* input,
                                  audio_chunk_t* output);

/// Update parameters for filters, mixers, and processors in the pipeline.
void pipeline_update_parameters(pipeline_t* pipeline,
                                const dsp_config_t* config,
                                const char* const* filters,
                                size_t filters_count, const char* const* mixers,
                                size_t mixers_count,
                                const char* const* processors,
                                size_t processors_count);

/// Destroy and free the pipeline.
void pipeline_free(pipeline_t* pipeline);

static inline size_t pipeline_get_last_error_needed(
    const pipeline_t* pipeline) {
  return pipeline ? pipeline->last_error_needed : 0;
}
static inline size_t pipeline_get_last_error_got(const pipeline_t* pipeline) {
  return pipeline ? pipeline->last_error_got : 0;
}

#ifdef __cplusplus
}
#endif

#endif  // CLIB_PIPELINE_PIPELINE_H
