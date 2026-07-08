/**
 * @file silence_counter.h
 * @brief Silence counter for automatic pause/resume based on signal level.
 *
 * This file defines the SilenceCounter, which tracks consecutive silent audio
 * chunks against a decibel threshold. It is used to determine if the audio
 * processing should be paused after a period of inactivity.
 */

#ifndef CLIB_AUDIO_SILENCE_COUNTER_H
#define CLIB_AUDIO_SILENCE_COUNTER_H

#include <stddef.h>

#include "Config/engine_config_types.h"

/**
 * @brief Opaque structure representing a silence counter.
 *
 * Counts consecutive silent chunks against a dB threshold and
 * reports back the desired engine state. `silence_counter_update(...)`
 * returns `PROCESSING_STATE_PAUSED` once silence has persisted for at least
 * the configured timeout, `PROCESSING_STATE_RUNNING` otherwise.
 *
 * Disabled when `timeout_seconds <= 0` — in that case `update`
 * always returns `PROCESSING_STATE_RUNNING`.
 */
typedef struct silence_counter silence_counter_t;

/**
 * @brief Creates a new silence counter instance.
 *
 * @param threshold_db The threshold in dB below which a chunk is considered
 * silent.
 * @param timeout_seconds The duration of silence in seconds before triggering a
 * pause.
 * @param samplerate The audio sample rate.
 * @param chunksize The size of each audio chunk in samples.
 * @return Pointer to the allocated silence_counter_t structure, or NULL on
 * failure.
 */
silence_counter_t* silence_counter_create(double threshold_db,
                                          double timeout_seconds,
                                          size_t samplerate, size_t chunksize);

/**
 * @brief Frees the silence counter instance.
 *
 * @param counter Pointer to the silence counter instance to free.
 */
void silence_counter_free(silence_counter_t* counter);

/**
 * @brief Initializes a silence counter instance.
 *
 * @param counter Pointer to the silence counter instance to initialize.
 * @param threshold_db The threshold in dB below which a chunk is considered
 * silent.
 * @param timeout_seconds The duration of silence in seconds before triggering a
 * pause.
 * @param samplerate The audio sample rate.
 * @param chunksize The size of each audio chunk in samples.
 */
void silence_counter_init(silence_counter_t* counter, double threshold_db,
                          double timeout_seconds, size_t samplerate,
                          size_t chunksize);

/**
 * @brief Feeds the next chunk's loudest channel peak (dB) to the counter.
 *
 * Updates the internal silent chunk count and returns the desired processing
 * state.
 *
 * @param counter Pointer to the silence counter.
 * @param signal_peak_db The peak signal level of the current chunk in dB.
 * @return The desired processing state (RUNNING or PAUSED).
 */
processing_state_t silence_counter_update(silence_counter_t* counter,
                                          double signal_peak_db);

/**
 * @brief Gets the limit of silent chunks before triggering a pause.
 *
 * @param counter Pointer to the silence counter.
 * @return The number of silent chunks that constitute the timeout.
 */
size_t silence_counter_get_limit_chunks(const silence_counter_t* counter);

/**
 * @brief Gets the current count of consecutive silent chunks.
 *
 * @param counter Pointer to the silence counter.
 * @return The current number of consecutive silent chunks.
 */
size_t silence_counter_get_silent_chunks(const silence_counter_t* counter);

#endif  // CLIB_AUDIO_SILENCE_COUNTER_H
