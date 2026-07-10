/**
 * @file audio_chunk.h
 * @brief Non-interleaved float buffers, one vector per channel.
 */

#ifndef CLIB_AUDIO_AUDIO_CHUNK_H
#define CLIB_AUDIO_AUDIO_CHUNK_H

#include "audio_buffers.h"

/**
 * @struct audio_chunk
 * @brief A chunk of non-interleaved audio data flowing through the pipeline.
 *
 * Storage is heap-backed (@ref audio_buffers_t) so per-channel mutable pointers
 * stay stable across struct copies and the audio thread can mutate samples
 * without going through copy-on-write uniqueness checks. Two @ref audio_chunk_t
 * values that share an @ref audio_buffers_t see the same samples — this is a
 * deliberate trade against value semantics, made to remove allocations on the
 * hot path.
 */
typedef struct audio_chunk audio_chunk_t;

/**
 * @struct round_robin_chunk_pool
 * @brief A pool of audio chunks for reuse to avoid allocations in the hot path.
 */
typedef struct round_robin_chunk_pool round_robin_chunk_pool_t;

/**
 * @brief Create a new silent audio chunk with freshly allocated storage.
 *
 * @param frames Number of frames (samples per channel).
 * @param channels Number of channels.
 * @return Pointer to the allocated audio_chunk_t, or NULL on failure.
 */
audio_chunk_t* audio_chunk_create(size_t frames, size_t channels);

/**
 * @brief Create an audio chunk that adopts the given audio_buffers_t.
 * Zero-copy.
 *
 * @param buffers The audio buffers to adopt.
 * @param valid_frames Number of valid frames in the buffers.
 * @return Pointer to the allocated audio_chunk_t, or NULL on failure.
 */
audio_chunk_t* audio_chunk_from_buffers(audio_buffers_t* buffers,
                                        size_t valid_frames);

/**
 * @brief Free the audio chunk.
 *
 * This will also free the underlying audio buffers if this chunk owns them.
 *
 * @param chunk Pointer to the audio_chunk_t to free.
 */
void audio_chunk_free(audio_chunk_t* chunk);

/**
 * @brief Get the sample capacity per channel of the chunk.
 *
 * @param chunk Pointer to the audio_chunk_t.
 * @return The number of frames capacity.
 */
size_t audio_chunk_get_frames(const audio_chunk_t* chunk);

/**
 * @brief Get the number of channels in the chunk.
 *
 * @param chunk Pointer to the audio_chunk_t.
 * @return The number of channels.
 */
size_t audio_chunk_get_channels(const audio_chunk_t* chunk);

/**
 * @brief Get a mutable pointer to the data for a specific channel.
 *
 * The pointer is stable for the lifetime of the underlying @ref audio_buffers_t
 * and aliases across struct copies — no CoW.
 *
 * @param chunk Pointer to the audio_chunk_t.
 * @param ch Channel index.
 * @return Mutable pointer to the channel's audio data.
 */
mutable_waveform_t audio_chunk_get_channel(const audio_chunk_t* chunk,
                                           size_t ch);

/**
 * @brief Get the number of valid frames in the chunk.
 *
 * May be less than @ref audio_chunk_get_frames at the end of a stream.
 *
 * @param chunk Pointer to the audio_chunk_t.
 * @return The number of valid frames.
 */
size_t audio_chunk_get_valid_frames(const audio_chunk_t* chunk);

/**
 * @brief Set the number of valid frames in the chunk.
 *
 * @param chunk Pointer to the audio_chunk_t.
 * @param valid_frames The number of valid frames to set.
 */
void audio_chunk_set_valid_frames(audio_chunk_t* chunk, size_t valid_frames);

/**
 * @brief Get the underlying audio buffers.
 *
 * @param chunk Pointer to the audio_chunk_t.
 * @return Pointer to the underlying @ref audio_buffers_t.
 */
audio_buffers_t* audio_chunk_get_buffers(audio_chunk_t* chunk);

/**
 * @brief Create a round-robin chunk pool.
 *
 * @param capacity The number of chunks in the pool.
 * @param frames The capacity of each chunk in frames.
 * @param channels The number of channels in each chunk.
 * @return Pointer to the created pool, or NULL on failure.
 */
round_robin_chunk_pool_t* round_robin_chunk_pool_create(size_t capacity,
                                                        size_t frames,
                                                        size_t channels);

/**
 * @brief Retrieves the next available unique chunk buffer from the pool.
 *
 * @param pool Pointer to the pool.
 * @return Pointer to an audio_chunk_t, or NULL if none are available.
 */
audio_chunk_t* round_robin_chunk_pool_next(round_robin_chunk_pool_t* pool);

/**
 * @brief Free the round-robin chunk pool and all its chunks.
 *
 * @param pool Pointer to the pool to free.
 */
void round_robin_chunk_pool_free(round_robin_chunk_pool_t* pool);

/**
 * @brief Sums multiple audio channels of a chunk into a single buffer.
 *
 * This function performs a vector addition to sum the samples from all
 * specified channels frame-by-frame, writing the result into the provided
 * output buffer. This is typically used to create a mono sum for sidechain
 * processing.
 *
 * @param chunk The audio chunk to sum from.
 * @param channels Array of channel indices to sum.
 * @param channels_count Number of channels in the channels array.
 * @param out_sum Output buffer to write the summed samples.
 * @param frames Number of frames to sum.
 */
void audio_chunk_sum_channels(const audio_chunk_t* chunk, const int* channels,
                              size_t channels_count, double* out_sum,
                              size_t frames);

/**
 * @brief Applies sample-by-sample linear gain to multiple channels in a chunk.
 *
 * Multiplies the wave samples of each specified channel by the corresponding
 * multiplier from the gain_multipliers buffer.
 *
 * @param chunk The audio chunk containing the channels to process.
 * @param channels Array of channel indices to apply gain to.
 * @param channels_count Number of channels in the channels array.
 * @param gain_multipliers Array of linear gain multipliers (one per frame).
 * @param frames Number of frames to process.
 */
void audio_chunk_apply_gain(audio_chunk_t* chunk, const int* channels,
                            size_t channels_count,
                            const double* gain_multipliers, size_t frames);

#endif  // CLIB_AUDIO_AUDIO_CHUNK_H
