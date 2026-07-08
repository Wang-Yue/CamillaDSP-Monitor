/**
 * @file file_backend.h
 * @brief File-based audio capture and playback backends.
 */

#ifndef CLIB_BACKEND_FILE_BACKEND_H
#define CLIB_BACKEND_FILE_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_backend.h"

/**
 * @brief Opaque structure representing the file capture backend.
 */
typedef struct file_capture file_capture_t;

/**
 * @brief Opaque structure representing the file playback backend.
 */
typedef struct file_playback file_playback_t;

/**
 * @brief Opaque structure representing processing parameters.
 */
typedef struct processing_parameters processing_parameters_t;

// Capture backend factory & methods

/**
 * @brief Create a file capture backend instance.
 *
 * @param config Pointer to the capture device configuration.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The size of each audio chunk in frames.
 * @param params Pointer to processing parameters.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return Pointer to the created capture_backend_t instance, or NULL on
 * failure.
 */
capture_backend_t* file_capture_create(const capture_device_config_t* config,
                                       int sample_rate, int chunk_size,
                                       processing_parameters_t* params,
                                       backend_error_t* err);

/**
 * @brief Open the file capture device.
 *
 * @param capture Pointer to the file capture instance.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool file_capture_open(file_capture_t* capture, backend_error_t* err);

/**
 * @brief Read audio frames from the file capture device.
 *
 * @param capture Pointer to the file capture instance.
 * @param frames Number of frames to read.
 * @param chunk Pointer to the audio chunk to fill.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool file_capture_read(file_capture_t* capture, size_t frames,
                       audio_chunk_t* chunk, backend_error_t* err);

/**
 * @brief Close the file capture device.
 *
 * @param capture Pointer to the file capture instance.
 */
void file_capture_close(file_capture_t* capture);

/**
 * @brief Get any pending sample rate change.
 *
 * @param capture Pointer to the file capture instance.
 * @param out_rate Pointer to double to store the pending sample rate.
 * @return true if a rate change is pending, false otherwise.
 */
bool file_capture_get_pending_rate_change(file_capture_t* capture,
                                          double* out_rate);

/**
 * @brief Check if pitch control is supported by the file capture backend.
 *
 * @param capture Pointer to the file capture instance.
 * @return true if supported, false otherwise.
 */
bool file_capture_pitch_control_supported(file_capture_t* capture);

/**
 * @brief Set the pitch multiplier for the file capture backend.
 *
 * @param capture Pointer to the file capture instance.
 * @param multiplier The pitch multiplier.
 */
void file_capture_set_pitch(file_capture_t* capture, double multiplier);

/**
 * @brief Wait for the file capture device to have data available.
 *
 * @param capture Pointer to the file capture instance.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if data is available, false on timeout or error.
 */
bool file_capture_wait(file_capture_t* capture, uint32_t timeout_ms);

/**
 * @brief Set the paused state of the file capture backend.
 *
 * @param capture Pointer to the file capture instance.
 * @param paused true to pause, false to resume.
 */
void file_capture_set_is_paused(file_capture_t* capture, bool paused);

/**
 * @brief Destroy the file capture backend instance.
 *
 * @param capture Pointer to the file capture instance to destroy.
 */
void file_capture_destroy(file_capture_t* capture);

// Playback backend factory & methods

/**
 * @brief Create a file playback backend instance.
 *
 * @param config Pointer to the playback device configuration.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The size of each audio chunk in frames.
 * @param params Pointer to processing parameters.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return Pointer to the created playback_backend_t instance, or NULL on
 * failure.
 */
playback_backend_t* file_playback_create(const playback_device_config_t* config,
                                         int sample_rate, int chunk_size,
                                         processing_parameters_t* params,
                                         backend_error_t* err);

/**
 * @brief Open the file playback device.
 *
 * @param playback Pointer to the file playback instance.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool file_playback_open(file_playback_t* playback, backend_error_t* err);

/**
 * @brief Write an audio chunk to the file playback device.
 *
 * @param playback Pointer to the file playback instance.
 * @param chunk Pointer to the audio chunk to write.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool file_playback_write(file_playback_t* playback, const audio_chunk_t* chunk,
                         backend_error_t* err);

/**
 * @brief Close the file playback device.
 *
 * @param playback Pointer to the file playback instance.
 */
void file_playback_close(file_playback_t* playback);

/**
 * @brief Get the current buffer level of the file playback backend.
 *
 * @param playback Pointer to the file playback instance.
 * @return The buffer level in samples.
 */
size_t file_playback_get_buffer_level(file_playback_t* playback);

/**
 * @brief Get any pending sample rate change.
 *
 * @param playback Pointer to the file playback instance.
 * @param out_rate Pointer to double to store the pending sample rate.
 * @return true if a rate change is pending, false otherwise.
 */
bool file_playback_get_pending_rate_change(file_playback_t* playback,
                                           double* out_rate);

/**
 * @brief Prefill the file playback buffer with silence.
 *
 * @param playback Pointer to the file playback instance.
 * @param frames Number of frames of silence to prefill.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool file_playback_prefill_silence(file_playback_t* playback, size_t frames,
                                   backend_error_t* err);

/**
 * @brief Check if file playback is currently paused.
 *
 * @param playback Pointer to the file playback instance.
 * @return true if paused, false otherwise.
 */
bool file_playback_get_is_paused(file_playback_t* playback);

/**
 * @brief Set the paused state of the file playback backend.
 *
 * @param playback Pointer to the file playback instance.
 * @param paused true to pause, false to resume.
 */
void file_playback_set_is_paused(file_playback_t* playback, bool paused);

/**
 * @brief Destroy the file playback backend instance.
 *
 * @param playback Pointer to the file playback instance to destroy.
 */
void file_playback_destroy(file_playback_t* playback);

#endif  // CLIB_BACKEND_FILE_BACKEND_H
