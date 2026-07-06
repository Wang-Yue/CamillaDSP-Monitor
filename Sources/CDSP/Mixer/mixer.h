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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Error codes returned by mixer processing functions.
 */
typedef enum {
  MIXER_OK = 0,
  /// `input.validFrames` is larger than the chunkSize the mixer was constructed
  /// with.
  MIXER_ERR_INPUT_SIZE_MISMATCH = -1,
  /// Caller's output AudioChunk doesn't have enough capacity per channel.
  MIXER_ERR_OUTPUT_BUFFER_TOO_SMALL = -2,
  /// Caller's output AudioChunk has the wrong channel count for this mixer.
  MIXER_ERR_CHANNEL_COUNT_MISMATCH = -3
} mixer_error_t;

/**
 * @brief Represents a prepared source channel contribution to a destination
 * channel.
 */
typedef struct {
  size_t in_channel;  ///< Input channel index.
  double gain;        ///< Linear gain multiplier (negative if inverted).
} prepared_source_t;

/**
 * @brief List of prepared source contributions for a single destination
 * channel.
 */
typedef struct {
  size_t count;                ///< Number of active contributing sources.
  prepared_source_t* sources;  ///< Array of prepared source contributions.
} prepared_source_list_t;

/// Mixer that changes channel count and routes/sums audio between channels.
typedef struct {
  size_t chunk_size;    ///< Maximum number of frames per processing chunk.
  char* name;           ///< Unique name of the mixer instance.
  size_t channels_in;   ///< Expected number of input channels.
  size_t channels_out;  ///< Number of output channels produced.
  prepared_source_list_t*
      mapping;  ///< Array of length channels_out defining source routing.
} audio_mixer_t;

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

/// Zero-allocation API. The caller pre-allocates `output` with
/// `output.channels == channelsOut` and `output.frames >= input.validFrames`.
/// The mixer writes the mixed samples directly and sets `output.validFrames`.
///
/// `input` and `output` must reference distinct buffers — the mixer
/// accumulates into the output and reads input concurrently, so aliasing
/// would corrupt the result.
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
 * @brief Updates mixer routing parameters dynamically.
 *
 * Re-evaluates the mapping matrix from the configuration and updates internal
 * prepared sources.
 *
 * @param mixer Pointer to audio mixer instance.
 * @param config New mixer configuration.
 */
void audio_mixer_update_parameters(audio_mixer_t* mixer,
                                   const mixer_config_t* config);

/**
 * @brief Frees all resources associated with the audio mixer.
 *
 * @param mixer Pointer to audio mixer instance to free.
 */
void audio_mixer_free(audio_mixer_t* mixer);

#ifdef __cplusplus
}
#endif

#endif  // CLIB_MIXER_MIXER_H
