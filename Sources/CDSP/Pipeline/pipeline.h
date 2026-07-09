/**
 * @file pipeline.h
 * @brief Main audio processing pipeline.
 *
 * This module manages the audio processing pipeline, including filters, mixers,
 * and processors. It handles processing audio chunks and updating parameters
 * dynamically.
 */

#ifndef CLIB_PIPELINE_PIPELINE_H
#define CLIB_PIPELINE_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Audio/processing_parameters.h"
#include "Config/config_error.h"
#include "Config/configuration.h"

/**
 * @brief Pipeline error codes.
 */
typedef enum {
  PIPELINE_OK = 0,                            ///< No error.
  PIPELINE_ERR_INPUT_SIZE_MISMATCH = -1,      ///< Input size mismatch.
  PIPELINE_ERR_OUTPUT_BUFFER_TOO_SMALL = -2,  ///< Output buffer too small.
  PIPELINE_ERR_CHANNEL_COUNT_MISMATCH = -3    ///< Channel count mismatch.
} pipeline_error_t;

struct pipeline_s;
/**
 * @brief Opaque structure representing the audio processing pipeline.
 */
typedef struct pipeline_s pipeline_t;

/**
 * @brief Initialize the main audio processing pipeline.
 *
 * @param[in] config The DSP configuration to initialize the pipeline with.
 * @param[in,out] proc_params Processing parameters.
 * @param[in] explicit_chunk_size Explicit chunk size, or 0 to use config
 * default.
 * @param[out] err Pointer to a config error struct to receive error details on
 * failure.
 * @return Pointer to the created pipeline, or NULL on failure.
 */
pipeline_t* pipeline_create(const dsp_config_t* config,
                            processing_parameters_t* proc_params,
                            size_t explicit_chunk_size, config_error_t* err);

/**
 * @brief Process an input audio chunk into an output audio chunk.
 *
 * @param[in,out] pipeline The pipeline instance.
 * @param[in] input The input audio chunk.
 * @param[out] output The output audio chunk.
 * @return pipeline_error_t error code.
 */
pipeline_error_t pipeline_process(pipeline_t* pipeline,
                                  const audio_chunk_t* input,
                                  audio_chunk_t* output);

/**
 * @brief Transfers all stateful filter/processor history variables from src to
 * dest pipeline.
 *
 * Scans the pipeline steps, matches filters/processors by name and type, and
 * transfers their internal states to prevent dynamic glitches on swaps.
 *
 * @param dest The destination pipeline instance (newly built).
 * @param src The source pipeline instance (currently active).
 */
void pipeline_transfer_state(pipeline_t* dest, const pipeline_t* src);

/**
 * @brief Destroy and free the pipeline.
 *
 * @param[in] pipeline The pipeline instance to free.
 */
void pipeline_free(pipeline_t* pipeline);

/**
 * @brief Get the expected number of channels for the last error.
 *
 * @param[in] pipeline The pipeline instance.
 * @return Expected number of channels.
 */
size_t pipeline_get_last_error_needed(const pipeline_t* pipeline);

/**
 * @brief Get the actual number of channels for the last error.
 *
 * @param[in] pipeline The pipeline instance.
 * @return Actual number of channels.
 */
size_t pipeline_get_last_error_got(const pipeline_t* pipeline);

#endif  // CLIB_PIPELINE_PIPELINE_H
