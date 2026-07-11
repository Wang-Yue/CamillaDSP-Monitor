/**
 * @file apple_resampler.h
 * @brief Resampler implementation using Apple's AudioToolbox AudioConverter.
 *
 * This module provides a wrapper around Apple's AudioConverter for performing
 * sample rate conversion on macOS/iOS.
 */

#ifndef CLIB_RESAMPLER_APPLE_RESAMPLER_H
#define CLIB_RESAMPLER_APPLE_RESAMPLER_H

#if defined(ENABLE_COREAUDIO)

#include <AudioToolbox/AudioToolbox.h>
#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Config/config_error.h"
#include "Config/resampler_config_types.h"
#include "resampler_error.h"

/**
 * @brief Internal fill context for the Apple AudioConverter callback.
 */
struct apple_resampler_fill_context;
typedef struct apple_resampler_fill_context apple_resampler_fill_context_t;

/**
 * @brief Apple resampler state structure.
 */
struct apple_resampler;
typedef struct apple_resampler apple_resampler_t;

/**
 * @brief Creates a new Apple AudioConverter resampler.
 *
 * @param channels Number of audio channels.
 * @param input_rate Input sample rate in Hz.
 * @param output_rate Output sample rate in Hz.
 * @param quality Resampling quality.
 * @param complexity Resampling complexity.
 * @param chunk_size Maximum number of frames per processing chunk.
 * @param err Pointer to a config error struct to populate on failure.
 * @return Pointer to newly allocated apple_resampler_t, or NULL on failure.
 */
apple_resampler_t* apple_resampler_create(
    size_t channels, size_t input_rate, size_t output_rate,
    apple_resampler_quality_t quality, apple_resampler_complexity_t complexity,
    size_t chunk_size, config_error_t* err);

/**
 * @brief Frees resources associated with the Apple resampler.
 *
 * @param resampler Pointer to the resampler to free.
 */
void apple_resampler_free(apple_resampler_t* resampler);

/**
 * @brief Processes an input audio chunk and writes the resampled output.
 *
 * @param resampler Pointer to the resampler.
 * @param input Pointer to the input audio chunk.
 * @param output Pointer to the output audio chunk where resampled data will be
 * written. The output chunk must have sufficient capacity.
 * @return RESAMPLER_SUCCESS on success, or an error code on failure.
 */
resampler_error_t apple_resampler_process(apple_resampler_t* resampler,
                                          const audio_chunk_t* input,
                                          audio_chunk_t* output);

/**
 * @brief Sets a relative resampling ratio multiplier.
 *
 * This can be used for runtime adjustments (e.g., drift compensation).
 *
 * @param resampler Pointer to the resampler.
 * @param multiplier Multiplier to apply to the nominal resampling ratio.
 */
void apple_resampler_set_relative_ratio(apple_resampler_t* resampler,
                                        double multiplier);

/**
 * @brief Gets the current effective resampling ratio (output_rate /
 * input_rate).
 *
 * @param resampler Pointer to the resampler.
 * @return The effective resampling ratio.
 */
double apple_resampler_get_ratio(const apple_resampler_t* resampler);

/**
 * @brief Gets the maximum number of output frames that can be generated for a
 * chunk.
 *
 * @param resampler Pointer to the resampler.
 * @return The maximum number of output frames.
 */
size_t apple_resampler_get_max_output_frames(
    const apple_resampler_t* resampler);

/**
 * @brief Gets the chunk size configured for the resampler.
 *
 * @param resampler Pointer to the resampler.
 * @return The chunk size in frames.
 */
size_t apple_resampler_get_chunk_size(const apple_resampler_t* resampler);

/**
 * @brief Gets the number of channels configured for the resampler.
 *
 * @param resampler Pointer to the resampler.
 * @return The number of channels.
 */
size_t apple_resampler_get_channels(const apple_resampler_t* resampler);

#endif  // ENABLE_COREAUDIO

#endif  // CLIB_RESAMPLER_APPLE_RESAMPLER_H
