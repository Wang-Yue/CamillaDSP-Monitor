#ifndef CLIB_FILTERS_DITHER_H
#define CLIB_FILTERS_DITHER_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/filter_config_types.h"

/**
 * @file dither.h
 * @brief Dithering and noise shaping filters.
 *
 * Provides structures and functions for applying dither and noise shaping
 * to audio signals.
 */

// MARK: - NoiseShaper

/**
 * @brief Opaque handle to a noise shaper instance.
 */
typedef struct noise_shaper noise_shaper_t;

/**
 * @brief Create a noise shaper with custom filter coefficients.
 *
 * @param filter_coeffs Array of filter coefficients.
 * @param count Number of coefficients in the array.
 * @return A pointer to the created noise shaper, or NULL on failure.
 */
noise_shaper_t* noise_shaper_create(const double* filter_coeffs, size_t count);

/**
 * @brief Process a single sample through the noise shaper.
 *
 * @param shaper The noise shaper instance.
 * @param scaled The scaled input sample (typically quantized to target bit
 * depth but still as double).
 * @param dither The dither value to add.
 * @return The noise-shaped sample.
 */
double noise_shaper_process(noise_shaper_t* shaper, double scaled,
                            double dither);

/**
 * @brief Free the noise shaper instance.
 *
 * @param shaper The noise shaper instance to free.
 */
void noise_shaper_free(noise_shaper_t* shaper);

// MARK: - Noise Shaper Factory

/**
 * @brief Create a noise shaper for a predefined dither type.
 *
 * @param type The dither type (e.g., standard, shaped).
 * @return A pointer to the created noise shaper, or NULL if the type doesn't
 * use noise shaping or on failure.
 */
noise_shaper_t* noise_shaper_create_for_type(dither_type_t type);

// MARK: - DitherFilter

/**
 * @brief Opaque handle to a dither filter instance.
 */
typedef struct dither_filter dither_filter_t;

/**
 * @brief Create a new dither filter.
 *
 * @param name The name of the filter.
 * @param params The dither parameters (type, bits, etc.).
 * @return A pointer to the created dither filter, or NULL on failure.
 */
dither_filter_t* dither_filter_create(const char* name,
                                      const dither_parameters_t* params);

/**
 * @brief Process a block of samples in-place, applying dither.
 *
 * @param filter The dither filter instance.
 * @param waveform The input/output waveform buffer.
 * @param count The number of samples to process.
 */
void dither_filter_process(dither_filter_t* filter, mutable_waveform_t waveform,
                           size_t count);

/**
 * @brief Free the dither filter instance and its associated resources.
 *
 * @param filter The dither filter instance to free.
 */
void dither_filter_free(dither_filter_t* filter);

#endif  // CLIB_FILTERS_DITHER_H
