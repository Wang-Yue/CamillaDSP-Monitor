/**
 * @file jack_backend.h
 * @brief JACK Audio Connection Kit capture and playback backends.
 */

#ifndef CLIB_BACKEND_JACK_BACKEND_H
#define CLIB_BACKEND_JACK_BACKEND_H

#if defined(ENABLE_JACK)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_backend.h"

/**
 * @brief Opaque structure representing the JACK capture backend.
 */
typedef struct jack_capture jack_capture_t;

/**
 * @brief Opaque structure representing the JACK playback backend.
 */
typedef struct jack_playback jack_playback_t;

/**
 * @brief Opaque structure representing processing parameters.
 */
typedef struct processing_parameters processing_parameters_t;

// Capture backend factory & methods

/**
 * @brief Create a JACK capture backend instance.
 *
 * @param config Pointer to the capture device configuration.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The size of each audio chunk in frames.
 * @param params Pointer to processing parameters.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return Pointer to the created capture_backend_t instance, or NULL on
 * failure.
 */
capture_backend_t* jack_capture_create(const capture_device_config_t* config,
                                       int sample_rate, int chunk_size,
                                       processing_parameters_t* params,
                                       backend_error_t* err);

/**
 * @brief Open the JACK capture device.
 *
 * @param capture Pointer to the JACK capture instance.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool jack_capture_open(jack_capture_t* capture, backend_error_t* err);

/**
 * @brief Read audio frames from the JACK capture device.
 *
 * @param capture Pointer to the JACK capture instance.
 * @param frames Number of frames to read.
 * @param chunk Pointer to the audio chunk to fill.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool jack_capture_read(jack_capture_t* capture, size_t frames,
                       audio_chunk_t* chunk, backend_error_t* err);

/**
 * @brief Close the JACK capture device.
 *
 * @param capture Pointer to the JACK capture instance.
 */
void jack_capture_close(jack_capture_t* capture);

/**
 * @brief Get any pending sample rate change.
 *
 * @param capture Pointer to the JACK capture instance.
 * @param out_rate Pointer to double to store the pending sample rate.
 * @return true if a rate change is pending, false otherwise.
 */
bool jack_capture_get_pending_rate_change(jack_capture_t* capture,
                                          double* out_rate);

/**
 * @brief Check if pitch control is supported by the JACK capture backend.
 *
 * @param capture Pointer to the JACK capture instance.
 * @return true if supported, false otherwise.
 */
bool jack_capture_pitch_control_supported(jack_capture_t* capture);

/**
 * @brief Set the pitch multiplier for the JACK capture backend.
 *
 * @param capture Pointer to the JACK capture instance.
 * @param multiplier The pitch multiplier.
 */
void jack_capture_set_pitch(jack_capture_t* capture, double multiplier);

/**
 * @brief Wait for the JACK capture device to have data available.
 *
 * @param capture Pointer to the JACK capture instance.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if data is available, false on timeout or error.
 */
bool jack_capture_wait(jack_capture_t* capture, uint32_t timeout_ms);

/**
 * @brief Destroy the JACK capture backend instance.
 *
 * @param capture Pointer to the JACK capture instance to destroy.
 */
void jack_capture_destroy(jack_capture_t* capture);

// Playback backend factory & methods

/**
 * @brief Create a JACK playback backend instance.
 *
 * @param config Pointer to the playback device configuration.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The size of each audio chunk in frames.
 * @param params Pointer to processing parameters.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return Pointer to the created playback_backend_t instance, or NULL on
 * failure.
 */
playback_backend_t* jack_playback_create(const playback_device_config_t* config,
                                         int sample_rate, int chunk_size,
                                         processing_parameters_t* params,
                                         backend_error_t* err);

/**
 * @brief Open the JACK playback device.
 *
 * @param playback Pointer to the JACK playback instance.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool jack_playback_open(jack_playback_t* playback, backend_error_t* err);

/**
 * @brief Write an audio chunk to the JACK playback device.
 *
 * @param playback Pointer to the JACK playback instance.
 * @param chunk Pointer to the audio chunk to write.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool jack_playback_write(jack_playback_t* playback, const audio_chunk_t* chunk,
                         backend_error_t* err);

/**
 * @brief Close the JACK playback device.
 *
 * @param playback Pointer to the JACK playback instance.
 */
void jack_playback_close(jack_playback_t* playback);

/**
 * @brief Get the current buffer level of the JACK playback backend.
 *
 * @param playback Pointer to the JACK playback instance.
 * @return The buffer level in samples.
 */
size_t jack_playback_get_buffer_level(jack_playback_t* playback);

/**
 * @brief Get any pending sample rate change.
 *
 * @param playback Pointer to the JACK playback instance.
 * @param out_rate Pointer to double to store the pending sample rate.
 * @return true if a rate change is pending, false otherwise.
 */
bool jack_playback_get_pending_rate_change(jack_playback_t* playback,
                                           double* out_rate);

/**
 * @brief Prefill the JACK playback buffer with silence.
 *
 * @param playback Pointer to the JACK playback instance.
 * @param frames Number of frames of silence to prefill.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool jack_playback_prefill_silence(jack_playback_t* playback, size_t frames,
                                   backend_error_t* err);

/**
 * @brief Check if JACK playback is currently paused.
 *
 * @param playback Pointer to the JACK playback instance.
 * @return true if paused, false otherwise.
 */
bool jack_playback_get_is_paused(jack_playback_t* playback);

/**
 * @brief Set the paused state of the JACK playback backend.
 *
 * @param playback Pointer to the JACK playback instance.
 * @param paused true to pause, false to resume.
 */
void jack_playback_set_is_paused(jack_playback_t* playback, bool paused);

/**
 * @brief Destroy the JACK playback backend instance.
 *
 * @param playback Pointer to the JACK playback instance to destroy.
 */
void jack_playback_destroy(jack_playback_t* playback);

#endif  // ENABLE_JACK

#endif  // CLIB_BACKEND_JACK_BACKEND_H
