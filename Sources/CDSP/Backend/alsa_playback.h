#ifndef CLIB_BACKEND_ALSA_PLAYBACK_H
#define CLIB_BACKEND_ALSA_PLAYBACK_H

#if defined(ENABLE_ALSA)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_backend.h"

/**
 * @file alsa_playback.h
 * @brief ALSA playback backend implementation.
 *
 * Implements the playback backend interface for ALSA (Advanced Linux Sound
 * Architecture) devices.
 */

/**
 * @struct alsa_playback
 * @brief Opaque structure representing the ALSA playback backend.
 */
typedef struct alsa_playback alsa_playback_t;

typedef struct processing_parameters processing_parameters_t;

/**
 * @brief Create an ALSA playback backend instance.
 *
 * @param config Configuration for the playback device.
 * @param sample_rate The nominal sample rate in Hz.
 * @param chunk_size The size of each audio chunk in frames.
 * @param params Opaque processing parameters pointer.
 * @param[out] err Pointer to store error details if creation fails.
 * @return A pointer to the created playback_backend_t interface wrapper, or
 * NULL on error.
 */
playback_backend_t* alsa_playback_create(const playback_device_config_t* config,
                                         int sample_rate, int chunk_size,
                                         processing_parameters_t* params,
                                         backend_error_t* err);

/**
 * @brief Open the ALSA playback device.
 *
 * Initializes and opens the ALSA PCM device for playback.
 *
 * @param playback Pointer to the ALSA playback instance.
 * @param[out] err Pointer to store error details if opening fails.
 * @return true if the device was successfully opened, false otherwise.
 */
bool alsa_playback_open(alsa_playback_t* playback, backend_error_t* err);

/**
 * @brief Write a chunk of audio to the ALSA device.
 *
 * @param playback Pointer to the ALSA playback instance.
 * @param chunk Pointer to the audio chunk to write.
 * @param[out] err Pointer to store error details if the write fails.
 * @return true on success, false on failure (e.g. xrun or write error).
 */
bool alsa_playback_write(alsa_playback_t* playback, const audio_chunk_t* chunk,
                         backend_error_t* err);

/**
 * @brief Close the ALSA playback device.
 *
 * Closes the ALSA PCM device and releases associated resources.
 *
 * @param playback Pointer to the ALSA playback instance.
 */
void alsa_playback_close(alsa_playback_t* playback);

/**
 * @brief Get the current buffer level of the ALSA device.
 *
 * Returns the number of frames currently in the ALSA playback buffer (delay).
 *
 * @param playback Pointer to the ALSA playback instance.
 * @return The buffer level in frames.
 */
size_t alsa_playback_get_buffer_level(alsa_playback_t* playback);

/**
 * @brief Check if there is a pending sample rate change on the ALSA device.
 *
 * @param playback Pointer to the ALSA playback instance.
 * @param[out] out_rate Pointer to store the new sample rate if a change is
 * pending.
 * @return true if a rate change was detected, false otherwise.
 */
bool alsa_playback_get_pending_rate_change(alsa_playback_t* playback,
                                           double* out_rate);

/**
 * @brief Prefill the ALSA playback buffer with silence.
 *
 * Writes the specified number of silence frames to the ALSA buffer.
 * This is used to prevent early underruns and establish an initial buffer
 * level.
 *
 * @param playback Pointer to the ALSA playback instance.
 * @param frames Number of silence frames to write.
 * @param[out] err Pointer to store error details if prefilling fails.
 * @return true on success, false on failure.
 */
bool alsa_playback_prefill_silence(alsa_playback_t* playback, size_t frames,
                                   backend_error_t* err);

/**
 * @brief Get the paused status of the ALSA playback.
 *
 * @param playback Pointer to the ALSA playback instance.
 * @return true if paused, false otherwise.
 */
bool alsa_playback_get_is_paused(alsa_playback_t* playback);

/**
 * @brief Set the paused status of the ALSA playback.
 *
 * @param playback Pointer to the ALSA playback instance.
 * @param paused true to pause, false to resume.
 */
void alsa_playback_set_is_paused(alsa_playback_t* playback, bool paused);

/**
 * @brief Check if the ALSA playback backend supports pitch (clock rate)
 * control.
 *
 * @param playback Pointer to the ALSA playback instance.
 * @return true if supported, false otherwise.
 */
bool alsa_playback_pitch_control_supported(alsa_playback_t* playback);

/**
 * @brief Set the pitch (clock multiplier) of the ALSA playback device.
 *
 * @param playback Pointer to the ALSA playback instance.
 * @param multiplier The clock rate multiplier (typically close to 1.0).
 */
void alsa_playback_set_pitch(alsa_playback_t* playback, double multiplier);

/**
 * @brief Destroy the ALSA playback backend.
 *
 * Frees all resources allocated by the ALSA playback backend.
 *
 * @param playback Pointer to the ALSA playback instance.
 */
void alsa_playback_destroy(alsa_playback_t* playback);

#endif  // ENABLE_ALSA

#endif  // CLIB_BACKEND_ALSA_PLAYBACK_H
