#ifndef CLIB_FILTERS_BIQUAD_COMBO_H
#define CLIB_FILTERS_BIQUAD_COMBO_H

/**
 * @file biquad_combo.h
 * @brief Combined biquad filters (e.g., Linkwitz-Riley, Butterworth, Tilt EQ,
 * Graphic EQ).
 *
 * This file provides interfaces for creating and managing filters that are
 * composed of multiple cascaded biquads, such as higher-order crossover
 * filters.
 */

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/config_error.h"
#include "Config/filter_config_types.h"

/**
 * @brief Opaque structure representing a combined biquad filter instance.
 */
typedef struct biquad_combo_filter biquad_combo_filter_t;

/**
 * @brief Validates combined biquad parameters.
 *
 * @param params The parameters to validate.
 * @param sample_rate The sample rate in Hz.
 * @param err Pointer to store error details if validation fails.
 * @return 0 if valid, non-zero error code otherwise.
 */
int biquad_combo_parameters_validate(const biquad_combo_parameters_t* params,
                                     int sample_rate, config_error_t* err);

/**
 * @brief Computes Q values for a Butterworth filter of a given order.
 *
 * @param order The filter order (must be positive).
 * @param out_q Array to store the computed Q values.
 * @param max_q Maximum capacity of the `out_q` array.
 * @return The number of Q values computed (number of biquad stages).
 */
size_t biquad_combo_butterworth_q(int order, double* out_q, size_t max_q);

/**
 * @brief Computes Q values for a Linkwitz-Riley filter of a given order.
 *
 * @param order The filter order (must be positive, typically even).
 * @param out_q Array to store the computed Q values.
 * @param max_q Maximum capacity of the `out_q` array.
 * @return The number of Q values computed (number of biquad stages).
 */
size_t biquad_combo_linkwitz_riley_q(int order, double* out_q, size_t max_q);

/**
 * @brief Creates a combined biquad filter instance.
 *
 * @param name The name of the filter.
 * @param params The parameters defining the filter type and characteristics.
 * @param sample_rate The sample rate in Hz.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A pointer to the created filter instance, or `NULL` on failure.
 */
biquad_combo_filter_t* biquad_combo_filter_create(
    const char* name, const biquad_combo_parameters_t* params, int sample_rate,
    config_error_t* err);

/**
 * @brief Processes an array of samples through the combined biquad filter.
 *
 * @param filter The filter instance.
 * @param waveform The input/output waveform buffer.
 * @param count The number of samples to process.
 */
void biquad_combo_filter_process(biquad_combo_filter_t* filter,
                                 mutable_waveform_t waveform, size_t count);

/**
 * @brief Transfers history state of nested biquad sections from src to dest.
 *
 * @param dest The destination combo filter instance.
 * @param src The source combo filter instance.
 */
void biquad_combo_filter_transfer_state(biquad_combo_filter_t* dest,
                                        const biquad_combo_filter_t* src);

/**
 * @brief Frees the combined biquad filter instance.
 *
 * @param filter The filter instance to free.
 */
void biquad_combo_filter_free(biquad_combo_filter_t* filter);

#endif  // CLIB_FILTERS_BIQUAD_COMBO_H
