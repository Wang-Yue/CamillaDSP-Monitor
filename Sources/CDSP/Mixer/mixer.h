#ifndef CLIB_MIXER_MIXER_H
#define CLIB_MIXER_MIXER_H

/**
 * @file mixer.h
 * @brief Channel routing matrix and audio mixing module.
 *
 * This module implements an audio mixer that changes channel counts and
 * routes/sums audio between channels according to a configurable routing
 * matrix.
 *
 * Channel Routing Matrix Explanation:
 * - A routing matrix defines mapping rules from source channels (input) to
 * destination channels (output).
 * - Each destination channel maintains a list of prepared sources that
 * contribute to its output.
 * - For each source channel contributing to a destination:
 *   - The gain can be specified in linear scale or decibels (dB). If specified
 * in dB, it is converted to linear gain: lin_gain = 10^(gain_db / 20).
 *   - If the source is marked as inverted, the linear gain is multiplied by
 * -1.0 to invert phase.
 *   - Muted sources or destination mappings are excluded from processing.
 * - During processing, destination channel buffers are cleared to zero and
 * source channels are summed in: if gain is 1.0, direct sample addition (add)
 * is performed; otherwise, multiply-add is performed using Apple Accelerate
 * (vDSP) or scalar fallback.
 * - ZERO-ALLOCATION GUARANTEE: Real-time audio processing
 * (`audio_mixer_process`) performs no memory allocations or deallocations. All
 * mapping tables and buffers are prepared during initialization
 * (`audio_mixer_create`) or parameter updates
 * (`audio_mixer_update_parameters`).
 */

#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Audio/double_helpers.h"
#include "Config/mixer_config_types.h"

/**
 * @brief Error codes returned by mixer processing functions.
 */
typedef enum {
  MIXER_OK = 0, /**< Success. */
  MIXER_ERR_INPUT_SIZE_MISMATCH =
      -1, /**< `input.validFrames` is larger than the chunkSize the mixer was
             constructed with. */
  MIXER_ERR_OUTPUT_BUFFER_TOO_SMALL =
      -2, /**< Caller's output AudioChunk doesn't have enough capacity per
             channel. */
  MIXER_ERR_CHANNEL_COUNT_MISMATCH =
      -3 /**< Caller's output AudioChunk has the wrong channel count for this
            mixer. */
} mixer_error_t;

/**
 * @brief Opaque struct representing an audio mixer instance.
 */
typedef struct audio_mixer_t audio_mixer_t;

/**
 * @brief Creates a new audio mixer instance from a configuration.
 *
 * @param name Unique name for this mixer instance.
 * @param config Mixer configuration containing channel counts and mapping
 * matrix.
 * @param chunk_size Maximum number of frames per chunk for processing.
 * @return Pointer to newly allocated audio_mixer_t, or NULL on failure.
 */
audio_mixer_t* audio_mixer_create(const char* name,
                                  const mixer_config_t* config,
                                  size_t chunk_size);

/**
 * @brief Zero-allocation API for mixing an audio chunk.
 *
 * The caller must pre-allocate the `output` chunk with:
 * - `output->channels == channelsOut`
 * - `output->frames >= input->validFrames`
 *
 * The mixer writes the mixed samples directly to the output and updates
 * `output->validFrames`.
 *
 * @note `input` and `output` must reference distinct buffers. The mixer
 * accumulates into the output and reads from the input concurrently; aliasing
 * (in-place processing) will corrupt the result.
 *
 * @param mixer Pointer to the audio mixer instance.
 * @param input Pointer to the input audio chunk.
 * @param output Pointer to the pre-allocated output audio chunk.
 * @return A mixer_error_t code representing success or failure.
 */
mixer_error_t audio_mixer_process(audio_mixer_t* mixer,
                                  const audio_chunk_t* input,
                                  audio_chunk_t* output);

/**
 * @brief Allocating convenience API for processing a chunk.
 *
 * Allocates a new output audio chunk and processes the input into it.
 * Note: This function allocates memory and should not be used on real-time
 * audio threads.
 *
 * @param mixer Pointer to audio mixer instance.
 * @param input Input audio chunk to process.
 * @return Newly allocated output audio chunk, or NULL on failure.
 */
audio_chunk_t* audio_mixer_process_chunk(audio_mixer_t* mixer,
                                         const audio_chunk_t* input);

/**
 * @brief Frees all resources associated with the audio mixer.
 *
 * @param mixer Pointer to audio mixer instance to free.
 */
void audio_mixer_free(audio_mixer_t* mixer);

/**
 * @brief Gets the number of expected input channels.
 *
 * @param mixer Pointer to audio mixer instance.
 * @return Number of input channels.
 */
size_t audio_mixer_get_channels_in(const audio_mixer_t* mixer);

/**
 * @brief Gets the number of output channels produced.
 *
 * @param mixer Pointer to audio mixer instance.
 * @return Number of output channels.
 */
size_t audio_mixer_get_channels_out(const audio_mixer_t* mixer);

/**
 * @brief Gets the name of the mixer instance.
 *
 * @param mixer Pointer to audio mixer instance.
 * @return Pointer to mixer name string.
 */
const char* audio_mixer_get_name(const audio_mixer_t* mixer);

#endif  // CLIB_MIXER_MIXER_H
