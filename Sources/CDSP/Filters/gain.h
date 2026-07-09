/**
 * @file gain.h
 * @brief Gain filter implementation.
 *
 * This file defines the functions to manage and apply a simple gain filter.
 */

#ifndef CLIB_FILTERS_GAIN_H
#define CLIB_FILTERS_GAIN_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/filter_config_types.h"

/**
 * @brief Opaque structure representing a gain filter.
 */
typedef struct gain_filter gain_filter_t;

/**
 * @brief Create a gain filter.
 *
 * @param name The name of the filter.
 * @param params Pointer to the gain parameters.
 * @return Pointer to the allocated gain_filter_t, or NULL on failure.
 */
gain_filter_t* gain_filter_create(const char* name,
                                  const gain_parameters_t* params);

/**
 * @brief Process a waveform buffer in-place by applying gain.
 *
 * @param filter Pointer to the gain filter instance.
 * @param waveform The waveform data to process.
 * @param count The number of samples to process.
 */
void gain_filter_process(gain_filter_t* filter, mutable_waveform_t waveform,
                         size_t count);

/**
 * @brief Process a single sample by applying gain.
 *
 * @param filter Pointer to the gain filter instance.
 * @param sample The input sample.
 * @return The gain-adjusted sample.
 */
double gain_filter_process_single(gain_filter_t* filter, double sample);

/**
 * @brief Free the gain filter instance.
 *
 * @param filter Pointer to the gain filter instance to free.
 */
void gain_filter_free(gain_filter_t* filter);

#endif  // CLIB_FILTERS_GAIN_H
