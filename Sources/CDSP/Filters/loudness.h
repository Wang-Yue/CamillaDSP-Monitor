#ifndef CLIB_FILTERS_LOUDNESS_H
#define CLIB_FILTERS_LOUDNESS_H

/**
 * @file loudness.h
 * @brief Loudness filter implementation based on RME ADI-2 DAC loudness curves.
 *
 * This module provides a loudness compensation filter that adjusts frequency
 * response according to RME ADI-2 DAC loudness curves.
 *
 * For more details, see: https://www.rme-audio.de/downloads/adi2dac_e.pdf
 */

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Audio/processing_parameters.h"
#include "Config/filter_config_types.h"

/**
 * @brief Opaque struct representing a loudness filter instance.
 */
typedef struct loudness_filter loudness_filter_t;

/**
 * @brief Creates a new loudness filter instance.
 *
 * @param name The name of the filter.
 * @param params Pointer to the loudness filter configuration parameters.
 * @param sample_rate The sample rate of the audio processing path.
 * @param proc_params Pointer to shared processing parameters.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A pointer to the newly created loudness_filter_t instance, or NULL on
 * failure.
 */
loudness_filter_t* loudness_filter_create(const char* name,
                                          const loudness_parameters_t* params,
                                          int sample_rate,
                                          processing_parameters_t* proc_params,
                                          config_error_t* err);

/**
 * @brief Processes a slice of waveform using the loudness filter.
 *
 * @param filter Pointer to the loudness filter instance.
 * @param waveform The waveform containing the samples to be processed.
 * @param count The number of samples to process.
 */
void loudness_filter_process(loudness_filter_t* filter,
                             mutable_waveform_t waveform, size_t count);

/**
 * @brief Transfers nested shelf filter states and last volume level from src to
 * dest.
 *
 * @param dest The destination loudness filter instance.
 * @param src The source loudness filter instance.
 */
void loudness_filter_transfer_state(loudness_filter_t* dest,
                                    const loudness_filter_t* src);

/**
 * @brief Frees the resources allocated for the loudness filter instance.
 *
 * @param filter Pointer to the loudness filter instance to free.
 */
void loudness_filter_free(loudness_filter_t* filter);

#endif  // CLIB_FILTERS_LOUDNESS_H
