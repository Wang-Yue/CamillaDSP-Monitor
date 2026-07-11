/**
 * @file filter.h
 * @brief Generic filter interface and factory.
 *
 * This file defines the generic `filter_t` structure, which wraps various
 * filter implementations, and the functions to manage them.
 */

#ifndef CLIB_FILTERS_FILTER_H
#define CLIB_FILTERS_FILTER_H

#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Audio/processing_parameters.h"
#include "Config/filter_config_types.h"

/**
 * @brief Types of filter instances.
 */
typedef enum {
  FILTER_INSTANCE_BIQUAD,            /**< Biquad filter. */
  FILTER_INSTANCE_BIQUAD_COMBO,      /**< Combination of biquad filters. */
  FILTER_INSTANCE_CONVOLUTION,       /**< Convolution filter (FIR). */
  FILTER_INSTANCE_DELAY,             /**< Delay filter. */
  FILTER_INSTANCE_DIFF_EQ,           /**< Difference equation filter. */
  FILTER_INSTANCE_DITHER,            /**< Dither filter. */
  FILTER_INSTANCE_GAIN,              /**< Gain filter. */
  FILTER_INSTANCE_LIMITER,           /**< Limiter filter. */
  FILTER_INSTANCE_LOOKAHEAD_LIMITER, /**< Lookahead limiter filter. */
  FILTER_INSTANCE_LOUDNESS,          /**< Loudness filter. */
  FILTER_INSTANCE_VOLUME             /**< Volume filter. */
} filter_instance_type_t;

/**
 * @brief Representation of a filter.
 *
 * Filters operate on one channel at a time.
 */
typedef struct filter filter_t;

/**
 * @brief Factory to create filter instances from configuration.
 *
 * Validation runs first via `FilterConfig.validate(sampleRate:)`; the
 * switch then constructs the runtime filter for each variant. The
 * `.volume` case is reserved for the implicit master-volume filter
 * inside `Pipeline` and cannot be user-defined.
 *
 * @param name The name of the filter.
 * @param config Pointer to the filter configuration.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The processing chunk size.
 * @param proc_params Pointer to shared processing parameters.
 * @param err Pointer to a config error struct to populate on failure.
 * @return Pointer to the allocated filter_t structure, or NULL on failure.
 */
filter_t* filter_create(const char* name, const filter_config_t* config,
                        int sample_rate, size_t chunk_size,
                        processing_parameters_t* proc_params,
                        config_error_t* err);

/**
 * @brief Process a waveform buffer in-place.
 *
 * The buffer's `count` defines the processed range.
 * `waveform` is a pointer into class-owned storage (`AudioBuffers`). The
 * pointer's `count` is the number of samples to process — typically the
 * owning chunk's `validFrames`, sliced down by the caller. Filters must
 * not assume the pointer covers the channel's full capacity.
 *
 * @param filter Pointer to the filter instance.
 * @param waveform The waveform data to process in-place.
 * @param count The number of samples to process.
 */
void filter_process(filter_t* filter, mutable_waveform_t waveform,
                    size_t count);

/**
 * @brief Transfers history state from src to dest generic filter wrapper.
 *
 * Only applies to stateful filter types (Biquad, BiquadCombo, Loudness,
 * Volume).
 *
 * @param dest The destination filter wrapper instance.
 * @param src The source filter wrapper instance.
 */
void filter_transfer_state(filter_t* dest, const filter_t* src);

/**
 * @brief Gets the unique name of this filter instance.
 *
 * @param filter Pointer to the filter wrapper instance.
 * @return The unique name of the filter.
 */
const char* filter_get_name(const filter_t* filter);

/**
 * @brief Free the filter instance and its resources.
 *
 * @param filter Pointer to the filter instance to free.
 */
void filter_free(filter_t* filter);

#endif  // CLIB_FILTERS_FILTER_H
