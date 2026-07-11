#ifndef CLIB_FILTERS_DELAY_H
#define CLIB_FILTERS_DELAY_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/filter_config_types.h"

/**
 * @file delay.h
 * @brief Delay filter with optional subsample interpolation.
 *
 * Supports integer sample delay and subsample delay using Thiran allpass
 * biquads (1st or 2nd order).
 */

/**
 * @brief Opaque handle to a delay filter instance.
 */
typedef struct delay_filter delay_filter_t;

/**
 * @brief Create a new delay filter.
 *
 * Builds the subsample biquad allpass filter if fractional delay is requested.
 * 1st order Thiran allpass: coeffs a1 = coeff, b0 = coeff, b1 = 1.0, b2 = 0.0,
 * a2 = 0.0 2nd order Thiran allpass is also supported.
 *
 * @param name The name of the filter.
 * @param params The delay parameters (delay value, unit, etc.).
 * @param sample_rate The audio sample rate in Hz.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A pointer to the created delay filter, or NULL on failure.
 */
delay_filter_t* delay_filter_create(const char* name,
                                    const delay_parameters_t* params,
                                    int sample_rate,
                                    config_error_t* err);

/**
 * @brief Process a block of samples in-place.
 *
 * @param filter The delay filter instance.
 * @param waveform The input/output waveform buffer.
 * @param count The number of samples to process.
 */
void delay_filter_process(delay_filter_t* filter, mutable_waveform_t waveform,
                          size_t count);

/**
 * @brief Process a single sample.
 *
 * @param filter The delay filter instance.
 * @param sample The input sample.
 * @return The delayed sample.
 */
double delay_filter_process_single(delay_filter_t* filter, double sample);

/**
 * @brief Free the delay filter instance and its associated resources.
 *
 * @param filter The delay filter instance to free.
 */
void delay_filter_free(delay_filter_t* filter);

#endif  // CLIB_FILTERS_DELAY_H
