/**
 * @file async_poly_resampler.h
 * @brief Asynchronous polynomial resampler.
 *
 * Provides polynomial-based resampling (linear, cubic, quintic, septic).
 * This resampler does not perform anti-aliasing, making it suitable when
 * CPU resources are constrained and high-frequency aliasing is acceptable,
 * or for low resampling ratios.
 *
 * Memory is allocated only during initialization.
 */

#ifndef CLIB_RESAMPLER_ASYNC_POLY_RESAMPLER_H
#define CLIB_RESAMPLER_ASYNC_POLY_RESAMPLER_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Config/config_error.h"
#include "resampler_error.h"

/**
 * @brief Types of polynomial interpolation supported.
 */
typedef enum {
  /** Linear interpolation (requires 2 points). */
  POLY_INTERPOLATION_LINEAR = 0,
  /** Cubic interpolation (requires 4 points). */
  POLY_INTERPOLATION_CUBIC,
  /** Quintic interpolation (requires 6 points). */
  POLY_INTERPOLATION_QUINTIC,
  /** Septic interpolation (requires 8 points). */
  POLY_INTERPOLATION_SEPTIC
} poly_interpolation_t;

/**
 * @brief Helper function to get the number of points needed for an
 * interpolation type.
 *
 * @param interp The interpolation type.
 * @return The number of input points required.
 */
static inline int poly_interpolation_nbr_points(poly_interpolation_t interp) {
  switch (interp) {
    case POLY_INTERPOLATION_LINEAR:
      return 2;
    case POLY_INTERPOLATION_CUBIC:
      return 4;
    case POLY_INTERPOLATION_QUINTIC:
      return 6;
    case POLY_INTERPOLATION_SEPTIC:
      return 8;
    default:
      return 4;
  }
}

/**
 * @struct async_poly_resampler
 * @brief Opaque structure representing an asynchronous polynomial resampler.
 */
struct async_poly_resampler;
typedef struct async_poly_resampler async_poly_resampler_t;

/**
 * @brief Creates an asynchronous polynomial resampler.
 *
 * @param channels Number of audio channels.
 * @param input_rate Input sample rate in Hz.
 * @param output_rate Output sample rate in Hz.
 * @param interpolation The interpolation quality to use.
 * @param chunk_size Fixed number of input frames per process call.
 * @param max_relative_ratio Maximum relative ratio adjustment. Used for buffer
 * pre-allocation.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A new resampler instance, or NULL on failure.
 */
async_poly_resampler_t* async_poly_resampler_create(
    size_t channels, size_t input_rate, size_t output_rate,
    poly_interpolation_t interpolation, size_t chunk_size,
    double max_relative_ratio, config_error_t* err);

/**
 * @brief Frees the polynomial resampler resources.
 *
 * @param resampler The resampler instance to free.
 */
void async_poly_resampler_free(async_poly_resampler_t* resampler);

/**
 * @brief Processes a chunk of audio using polynomial interpolation.
 *
 * @param resampler The resampler instance.
 * @param input The input audio chunk.
 * @param output The output audio chunk.
 * @return @ref RESAMPLER_OK on success, or an error code.
 */
resampler_error_t async_poly_resampler_process(
    async_poly_resampler_t* resampler, const audio_chunk_t* input,
    audio_chunk_t* output);

/**
 * @brief Sets a relative ratio multiplier.
 *
 * @param resampler The resampler instance.
 * @param multiplier The ratio multiplier.
 */
void async_poly_resampler_set_relative_ratio(async_poly_resampler_t* resampler,
                                             double multiplier);

/**
 * @brief Gets the current effective ratio.
 *
 * @param resampler The resampler instance.
 * @return The effective resampling ratio.
 */
double async_poly_resampler_get_ratio(const async_poly_resampler_t* resampler);

/**
 * @brief Gets the maximum number of output frames that can be generated in one
 * call.
 *
 * @param resampler The resampler instance.
 * @return The maximum output frame count.
 */
size_t async_poly_resampler_get_max_output_frames(
    const async_poly_resampler_t* resampler);

/**
 * @brief Gets the configured chunk size.
 *
 * @param resampler The resampler instance.
 * @return The fixed input chunk size.
 */
size_t async_poly_resampler_get_chunk_size(
    const async_poly_resampler_t* resampler);

/**
 * @brief Gets the number of channels.
 *
 * @param resampler The resampler instance.
 * @return The channel count.
 */
size_t async_poly_resampler_get_channels(
    const async_poly_resampler_t* resampler);

#endif  // CLIB_RESAMPLER_ASYNC_POLY_RESAMPLER_H
