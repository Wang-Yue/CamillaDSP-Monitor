/**
 * @file lookahead_limiter.h
 * @brief Lookahead limiter filter implementation.
 *
 * This file defines the functions to manage and apply a lookahead limiter
 * filter.
 */

#ifndef CLIB_FILTERS_LOOKAHEAD_LIMITER_H
#define CLIB_FILTERS_LOOKAHEAD_LIMITER_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/config_error.h"
#include "Config/filter_config_types.h"

/**
 * @brief Opaque structure representing a lookahead limiter filter.
 */
typedef struct lookahead_limiter_filter lookahead_limiter_filter_t;

/**
 * @brief Validate lookahead limiter parameters.
 *
 * @param params Pointer to the parameters to validate.
 * @param sample_rate The sample rate in Hz.
 * @param err Pointer to a config error structure to populate on failure.
 * @return 0 on success, non-zero on failure.
 */
int lookahead_limiter_parameters_validate(
    const lookahead_limiter_parameters_t* params, int sample_rate,
    config_error_t* err);

/**
 * @brief Create a lookahead limiter filter.
 *
 * @param name The name of the filter.
 * @param params Pointer to the lookahead limiter parameters.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The processing chunk size.
 * @return Pointer to the allocated lookahead_limiter_filter_t, or NULL on
 * failure.
 */
lookahead_limiter_filter_t* lookahead_limiter_filter_create(
    const char* name, const lookahead_limiter_parameters_t* params,
    int sample_rate, size_t chunk_size);

/**
 * @brief Process a waveform buffer in-place by applying lookahead limiting.
 *
 * @param filter Pointer to the lookahead limiter filter instance.
 * @param waveform The waveform data to process.
 * @param count The number of samples to process.
 */
void lookahead_limiter_filter_process(lookahead_limiter_filter_t* filter,
                                      mutable_waveform_t waveform,
                                      size_t count);

/**
 * @brief Free the lookahead limiter filter instance.
 *
 * @param filter Pointer to the lookahead limiter filter instance to free.
 */
void lookahead_limiter_filter_free(lookahead_limiter_filter_t* filter);

#endif  // CLIB_FILTERS_LOOKAHEAD_LIMITER_H
