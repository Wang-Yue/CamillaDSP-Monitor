#ifndef CLIB_PIPELINE_PIPELINE_H
#define CLIB_PIPELINE_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Audio/processing_parameters.h"
#include "Config/config_error.h"
#include "Config/configuration.h"

typedef enum {
  PIPELINE_OK = 0,
  PIPELINE_ERR_INPUT_SIZE_MISMATCH = -1,
  PIPELINE_ERR_OUTPUT_BUFFER_TOO_SMALL = -2,
  PIPELINE_ERR_CHANNEL_COUNT_MISMATCH = -3
} pipeline_error_t;

struct pipeline_s;
typedef struct pipeline_s pipeline_t;

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

size_t pipeline_get_last_error_needed(const pipeline_t* pipeline);
size_t pipeline_get_last_error_got(const pipeline_t* pipeline);

#endif  // CLIB_PIPELINE_PIPELINE_H
