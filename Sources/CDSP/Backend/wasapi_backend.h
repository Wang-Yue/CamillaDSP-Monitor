/**
 * @file wasapi_backend.h
 * @brief WASAPI backend for audio capture and playback.
 *
 * This file provides the interface for creating and managing WASAPI
 * capture and playback backends.
 */

#ifndef CLIB_BACKEND_WASAPI_BACKEND_H
#define CLIB_BACKEND_WASAPI_BACKEND_H

#if defined(ENABLE_WASAPI)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_backend.h"

/**
 * @typedef wasapi_capture_t
 * @brief Opaque structure representing a WASAPI capture session.
 */
typedef struct wasapi_capture wasapi_capture_t;

/**
 * @typedef wasapi_playback_t
 * @brief Opaque structure representing a WASAPI playback session.
 */
typedef struct wasapi_playback wasapi_playback_t;

/**
 * @typedef processing_parameters_t
 * @brief Opaque structure representing processing parameters.
 */
typedef struct processing_parameters processing_parameters_t;

// Capture backend factory & methods

/**
 * @brief Creates a WASAPI capture backend.
 *
 * @param config Configuration for the capture device.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The chunk size in frames.
 * @param params Processing parameters.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return A pointer to the created capture_backend_t, or NULL on failure.
 */
capture_backend_t* wasapi_capture_create(const capture_device_config_t* config,
                                         int sample_rate, int chunk_size,
                                         processing_parameters_t* params,
                                         backend_error_t* err);

/**
 * @brief Opens the WASAPI capture stream.
 *
 * @param capture Pointer to the wasapi_capture_t instance.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return true if successful, false otherwise.
 */
bool wasapi_capture_open(wasapi_capture_t* capture, backend_error_t* err);

/**
 * @brief Reads audio data from the WASAPI capture stream.
 *
 * @param capture Pointer to the wasapi_capture_t instance.
 * @param frames Number of frames to read.
 * @param chunk Pointer to the audio_chunk_t where the read data will be stored.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return true if successful, false otherwise.
 */
bool wasapi_capture_read(wasapi_capture_t* capture, size_t frames,
                         audio_chunk_t* chunk, backend_error_t* err);

/**
 * @brief Closes the WASAPI capture stream.
 *
 * @param capture Pointer to the wasapi_capture_t instance.
 */
void wasapi_capture_close(wasapi_capture_t* capture);

/**
 * @brief Checks if there is a pending rate change for the capture backend.
 *
 * @param capture Pointer to the wasapi_capture_t instance.
 * @param out_rate Pointer to double to receive the new rate if pending.
 * @return true if there is a pending rate change, false otherwise.
 */
bool wasapi_capture_get_pending_rate_change(wasapi_capture_t* capture,
                                            double* out_rate);

/**
 * @brief Checks if pitch control is supported by the capture backend.
 *
 * @param capture Pointer to the wasapi_capture_t instance.
 * @return true if supported, false otherwise.
 */
bool wasapi_capture_pitch_control_supported(wasapi_capture_t* capture);

/**
 * @brief Sets the pitch multiplier for the capture backend.
 *
 * @param capture Pointer to the wasapi_capture_t instance.
 * @param multiplier The pitch multiplier to set.
 */
void wasapi_capture_set_pitch(wasapi_capture_t* capture, double multiplier);

/**
 * @brief Waits for audio data to be available for capture.
 *
 * @param capture Pointer to the wasapi_capture_t instance.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if data is available, false on timeout or error.
 */
bool wasapi_capture_wait(wasapi_capture_t* capture, uint32_t timeout_ms);

/**
 * @brief Destroys the WASAPI capture instance and frees resources.
 *
 * @param capture Pointer to the wasapi_capture_t instance to destroy.
 */
void wasapi_capture_destroy(wasapi_capture_t* capture);

// Playback backend factory & methods

/**
 * @brief Creates a WASAPI playback backend.
 *
 * @param config Configuration for the playback device.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The chunk size in frames.
 * @param params Processing parameters.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return A pointer to the created playback_backend_t, or NULL on failure.
 */
playback_backend_t* wasapi_playback_create(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    processing_parameters_t* params, backend_error_t* err);

/**
 * @brief Opens the WASAPI playback stream.
 *
 * @param playback Pointer to the wasapi_playback_t instance.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return true if successful, false otherwise.
 */
bool wasapi_playback_open(wasapi_playback_t* playback, backend_error_t* err);

/**
 * @brief Writes audio data to the WASAPI playback stream.
 *
 * @param playback Pointer to the wasapi_playback_t instance.
 * @param chunk Pointer to the audio_chunk_t containing data to write.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return true if successful, false otherwise.
 */
bool wasapi_playback_write(wasapi_playback_t* playback,
                           const audio_chunk_t* chunk, backend_error_t* err);

/**
 * @brief Closes the WASAPI playback stream.
 *
 * @param playback Pointer to the wasapi_playback_t instance.
 */
void wasapi_playback_close(wasapi_playback_t* playback);

/**
 * @brief Gets the current buffer level of the playback backend in frames.
 *
 * @param playback Pointer to the wasapi_playback_t instance.
 * @return The buffer level in frames.
 */
size_t wasapi_playback_get_buffer_level(wasapi_playback_t* playback);

/**
 * @brief Checks if there is a pending rate change for the playback backend.
 *
 * @param playback Pointer to the wasapi_playback_t instance.
 * @param out_rate Pointer to double to receive the new rate if pending.
 * @return true if there is a pending rate change, false otherwise.
 */
bool wasapi_playback_get_pending_rate_change(wasapi_playback_t* playback,
                                             double* out_rate);

/**
 * @brief Prefills the playback buffer with silence.
 *
 * @param playback Pointer to the wasapi_playback_t instance.
 * @param frames Number of frames of silence to prefill.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return true if successful, false otherwise.
 */
bool wasapi_playback_prefill_silence(wasapi_playback_t* playback, size_t frames,
                                     backend_error_t* err);

/**
 * @brief Checks if the playback is currently paused.
 *
 * @param playback Pointer to the wasapi_playback_t instance.
 * @return true if paused, false otherwise.
 */
bool wasapi_playback_get_is_paused(wasapi_playback_t* playback);

/**
 * @brief Sets the paused state of the playback.
 *
 * @param playback Pointer to the wasapi_playback_t instance.
 * @param paused true to pause, false to resume.
 */
void wasapi_playback_set_is_paused(wasapi_playback_t* playback, bool paused);

/**
 * @brief Destroys the WASAPI playback instance and frees resources.
 *
 * @param playback Pointer to the wasapi_playback_t instance to destroy.
 */
void wasapi_playback_destroy(wasapi_playback_t* playback);

#endif  // ENABLE_WASAPI

#endif  // CLIB_BACKEND_WASAPI_BACKEND_H
