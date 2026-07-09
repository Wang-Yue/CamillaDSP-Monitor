#ifndef CLIB_FILTERS_DIFFEQ_H
#define CLIB_FILTERS_DIFFEQ_H

#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/filter_config_types.h"

/**
 * @file diffeq.h
 * @brief Difference equation filter (IIR/FIR filter implementation).
 *
 * Implements a difference equation filter using Direct Form I or II.
 * Coefficients are normalized by a[0].
 */

/**
 * @brief Opaque handle to a difference equation filter instance.
 */
typedef struct diffeq_filter diffeq_filter_t;

/**
 * @brief Create a new difference equation filter.
 *
 * @param name The name of the filter.
 * @param params The difference equation parameters (coefficients feedforward
 * 'b' and feedback 'a').
 * @return A pointer to the created difference equation filter, or NULL on
 * failure.
 */
diffeq_filter_t* diffeq_filter_create(const char* name,
                                      const diff_eq_parameters_t* params);

/**
 * @brief Process a block of samples in-place.
 *
 * @param filter The difference equation filter instance.
 * @param waveform The input/output waveform buffer.
 * @param count The number of samples to process.
 */
void diffeq_filter_process(diffeq_filter_t* filter, mutable_waveform_t waveform,
                           size_t count);

/**
 * @brief Free the difference equation filter instance and its associated
 * resources.
 *
 * @param filter The difference equation filter instance to free.
 */
void diffeq_filter_free(diffeq_filter_t* filter);

#endif  // CLIB_FILTERS_DIFFEQ_H
