/**
 * @file audio_history_buffer.h
 * @brief Stores recent audio samples for spectrum analysis and vector scope.
 */

#ifndef CLIB_AUDIO_AUDIO_HISTORY_BUFFER_H
#define CLIB_AUDIO_AUDIO_HISTORY_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Audio/lock_free_ring_buffer.h"

/**
 * @def AUDIO_HISTORY_BUFFER_CAPACITY
 * @brief Maximum number of frames retained per channel.
 *
 * At 48 kHz that's roughly 5.5 s of audio — enough headroom for an FFT down to
 * ~5 Hz.
 */
#define AUDIO_HISTORY_BUFFER_CAPACITY 262144

/**
 * @enum audio_history_buffer_status
 * @brief Status codes returned by @ref audio_history_buffer_read_latest.
 *
 * The buffer is general-purpose (used by spectrum analysis *and* level/sample
 * queries), so it owns its own error/status type rather than borrowing from the
 * spectrum analyzer.
 */
typedef enum {
  /** Success. */
  AUDIO_HISTORY_BUFFER_OK = 0,
  /** @ref audio_history_buffer_reset has not been called, or was called with
     `0`. */
  AUDIO_HISTORY_BUFFER_ERROR_EMPTY = -1,
  /** Caller asked for a channel index outside `0..<channels`. */
  AUDIO_HISTORY_BUFFER_ERROR_OUT_OF_RANGE = -2
} audio_history_buffer_status_t;

/**
 * @struct audio_history_buffer
 * @brief Owns one @ref spsc_audio_ring_buffer_t per channel.
 *
 * Resized only between engine starts, when no audio thread is running.
 * Read by consumers via @ref audio_history_buffer_read_latest (snapshot
 * semantics — same window can be re-read for FFTs at different lengths),
 * optionally averaging across channels.
 */
typedef struct audio_history_buffer audio_history_buffer_t;

/**
 * @brief Create a new audio history buffer.
 *
 * @return Pointer to the allocated audio_history_buffer_t, or NULL on failure.
 */
audio_history_buffer_t* audio_history_buffer_create(void);

/**
 * @brief Re-allocate buffers for a new channel layout.
 *
 * Must only be called while the engine is stopped (no producer touching the
 * ring).
 *
 * @param history Pointer to the history buffer.
 * @param channels Number of channels to allocate.
 */
void audio_history_buffer_reset(audio_history_buffer_t* history,
                                size_t channels);

/**
 * @brief Free the audio history buffer.
 *
 * @param history Pointer to the history buffer to free.
 */
void audio_history_buffer_free(audio_history_buffer_t* history);

/**
 * @brief Get the number of channels.
 *
 * @param history Pointer to the history buffer.
 * @return The number of channels.
 */
size_t audio_history_buffer_get_channels(const audio_history_buffer_t* history);

/**
 * @brief Check if any sample has been written on this side yet.
 *
 * @param history Pointer to the history buffer.
 * @return true if data has been written, false otherwise.
 */
bool audio_history_buffer_has_data(const audio_history_buffer_t* history);

/**
 * @brief Append audio chunk to the history buffer.
 *
 * **Producer-only.** Forward each channel's waveform into the matching
 * lock-free ring.
 *
 * @param history Pointer to the history buffer.
 * @param chunk Pointer to the audio chunk to append.
 */
void audio_history_buffer_append(audio_history_buffer_t* history,
                                 const audio_chunk_t* chunk);

/**
 * @brief Copy the most recent samples.
 *
 * **Consumer.** Copy the most recent `count` samples for the given
 * channel into `dest`. When `channel` is negative (`-1`), all channels are
 * averaged into `dest`. Returns status code and sets `*enough_data` to `false`
 * if there isn't enough data yet.
 *
 * `dest` must have capacity for at least `count` floats.
 *
 * @param history Pointer to the history buffer.
 * @param dest Destination buffer.
 * @param count Number of samples to read.
 * @param channel Channel index, or -1 to average all channels.
 * @param enough_data Output flag indicating if enough data was available.
 * @return Status code @ref audio_history_buffer_status_t.
 */
audio_history_buffer_status_t audio_history_buffer_read_latest(
    const audio_history_buffer_t* history, float* dest, size_t count,
    int channel, bool* enough_data);

#endif  // CLIB_AUDIO_AUDIO_HISTORY_BUFFER_H
