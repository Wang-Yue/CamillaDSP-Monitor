/**
 * @file audio_buffers.h
 * @brief Heap-allocated, contiguous per-channel audio storage.
 *
 * Replaces nested array/pointer-to-pointer ("array of arrays") chunk storage.
 * The 2-D nested layout had two costs the audio thread couldn't afford:
 *
 * 1. Uniqueness/copy-on-write checks or fragmented heap access on inner
 *    buffers; whenever any external reference kept an inner buffer shared
 *    (closures, queues, captures), mutable access risked malloc'ing a fresh
 *    copy or causing cache misses.
 * 2. Element copies bumped per-channel buffer refcounts or overhead
 *    on every value-copy of an audio chunk.
 *
 * @ref audio_buffers_t allocates one contiguous block of `channels * capacity`
 * `double` values up front and exposes per-channel @ref mutable_waveform_t
 * (pointer) views that are stable for the buffer's lifetime. The hot path uses
 * the pointers directly — no pointer round trips, no ownership/uniqueness
 * checks.
 *
 * Thread-safety: @ref audio_buffers_t itself does no synchronisation. The
 * pipeline already enforces single-writer discipline (the audio thread owns
 * each buffer while it processes a chunk), so lock-free single-owner usage is
 * honest here — the type is safe under pipeline discipline without locking.
 */

#ifndef CLIB_AUDIO_AUDIO_BUFFERS_H
#define CLIB_AUDIO_AUDIO_BUFFERS_H

#include <stddef.h>

#include "double_helpers.h"

/**
 * @struct audio_buffers
 * @brief Contiguous, per-channel audio storage backed by a single heap
 * allocation.
 */
typedef struct audio_buffers audio_buffers_t;

/**
 * @brief Allocate a fresh buffer pool, zero-initialised.
 *
 * @param channels Number of audio channels.
 * @param capacity Capacity in samples per channel.
 * @return Pointer to the allocated audio_buffers_t, or NULL on failure.
 */
audio_buffers_t* audio_buffers_create(size_t channels, size_t capacity);

/**
 * @brief Allocate a fresh buffer pool and copy existing waveform data into it.
 *
 * @param waveforms Array of pointers to channel data.
 * @param channel_lengths Array of lengths for each channel.
 * @param channels Number of channels.
 * @return Pointer to the allocated audio_buffers_t, or NULL on failure.
 */
audio_buffers_t* audio_buffers_copy_from(const double* const* waveforms,
                                         const size_t* channel_lengths,
                                         size_t channels);

/**
 * @brief Free the audio buffers.
 *
 * @param buffers Pointer to the audio_buffers_t to free.
 */
void audio_buffers_free(audio_buffers_t* buffers);

/**
 * @brief Get the number of channels in the buffer.
 *
 * @param buffers Pointer to the audio_buffers_t.
 * @return The number of channels.
 */
size_t audio_buffers_get_channels(const audio_buffers_t* buffers);

/**
 * @brief Get the capacity in samples per channel.
 *
 * @param buffers Pointer to the audio_buffers_t.
 * @return The capacity in samples per channel.
 */
size_t audio_buffers_get_capacity(const audio_buffers_t* buffers);

/**
 * @brief Get a mutable pointer to the data for a specific channel.
 *
 * The pointer is stable for the lifetime of the @ref audio_buffers_t;
 * callers may cache it.
 *
 * @param buffers Pointer to the audio_buffers_t.
 * @param ch Channel index.
 * @return Mutable pointer to the channel's audio data.
 */
mutable_waveform_t audio_buffers_get_channel(const audio_buffers_t* buffers,
                                             size_t ch);

#endif  // CLIB_AUDIO_AUDIO_BUFFERS_H
