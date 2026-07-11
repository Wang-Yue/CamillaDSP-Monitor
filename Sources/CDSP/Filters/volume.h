#ifndef CLIB_FILTERS_VOLUME_H
#define CLIB_FILTERS_VOLUME_H

/**
 * @file volume.h
 * @brief Volume filter implementation with ramping/fading support.
 *
 * This module provides a volume adjustment filter that can apply gain and
 * smoothly transition (ramp/fade) between different volume levels.
 */

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Audio/processing_parameters.h"
#include "Config/filter_config_types.h"

/**
 * @brief Opaque struct representing a volume filter instance.
 */
typedef struct volume_filter volume_filter_t;

/**
 * @brief Creates a new volume filter instance.
 *
 * @param name The name of the filter.
 * @param params Pointer to the volume filter configuration parameters.
 * @param sample_rate The sample rate of the audio processing path.
 * @param chunk_size The maximum size of an audio chunk in frames.
 * @param proc_params Pointer to shared processing parameters.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A pointer to the newly created volume_filter_t instance, or NULL on
 * failure.
 */
volume_filter_t* volume_filter_create(const char* name,
                                      const volume_parameters_t* params,
                                      int sample_rate, size_t chunk_size,
                                      processing_parameters_t* proc_params,
                                      config_error_t* err);

/**
 * @brief Pre-calculates target volume levels and generates ramping array once
 * per chunk.
 *
 * This function must be called once per audio chunk before processing
 * individual channel waveforms.
 *
 * @param filter Pointer to the volume filter instance.
 */
void volume_filter_prepare_chunk(volume_filter_t* filter);

/**
 * @brief Processes a single channel's waveform slice.
 *
 * Conforms to the `Filter` processing interface.
 *
 * @param filter Pointer to the volume filter instance.
 * @param waveform The waveform containing the samples to be processed.
 * @param count The number of samples to process.
 */
void volume_filter_process(volume_filter_t* filter, mutable_waveform_t waveform,
                           size_t count);

/**
 * @brief Advances the fader's ramp steps.
 *
 * Must be called once per audio chunk after all channels have been processed.
 *
 * @param filter Pointer to the volume filter instance.
 */
void volume_filter_advance_ramp(volume_filter_t* filter);

/**
 * @brief Transfers current volume, fader target level, and ramp state from src
 * to dest.
 *
 * @param dest The destination volume filter instance.
 * @param src The source volume filter instance.
 */
void volume_filter_transfer_state(volume_filter_t* dest,
                                  const volume_filter_t* src);

/**
 * @brief Frees the resources allocated for the volume filter instance.
 *
 * @param filter Pointer to the volume filter instance to free.
 */
void volume_filter_free(volume_filter_t* filter);

#endif  // CLIB_FILTERS_VOLUME_H
