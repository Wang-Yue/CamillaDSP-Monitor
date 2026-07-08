/**
 * @file pipewire_backend.h
 * @brief PipeWire capture and playback backends.
 */

#ifndef CLIB_BACKEND_PIPEWIRE_BACKEND_H
#define CLIB_BACKEND_PIPEWIRE_BACKEND_H

#if defined(ENABLE_PIPEWIRE)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_backend.h"

/**
 * @brief Opaque structure representing the PipeWire capture backend.
 */
typedef struct pipewire_capture pipewire_capture_t;

/**
 * @brief Opaque structure representing the PipeWire playback backend.
 */
typedef struct pipewire_playback pipewire_playback_t;

/**
 * @brief Opaque structure representing processing parameters.
 */
typedef struct processing_parameters processing_parameters_t;

// Capture backend factory & methods

/**
 * @brief Create a PipeWire capture backend instance.
 *
 * @param config Pointer to the capture device configuration.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The size of each audio chunk in frames.
 * @param params Pointer to processing parameters.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return Pointer to the created capture_backend_t instance, or NULL on
 * failure.
 */
capture_backend_t* pipewire_capture_create(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    processing_parameters_t* params, backend_error_t* err);

/**
 * @brief Open the PipeWire capture device.
 *
 * @param capture Pointer to the PipeWire capture instance.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool pipewire_capture_open(pipewire_capture_t* capture, backend_error_t* err);

/**
 * @brief Read audio frames from the PipeWire capture device.
 *
 * @param capture Pointer to the PipeWire capture instance.
 * @param frames Number of frames to read.
 * @param chunk Pointer to the audio chunk to fill.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool pipewire_capture_read(pipewire_capture_t* capture, size_t frames,
                           audio_chunk_t* chunk, backend_error_t* err);

/**
 * @brief Close the PipeWire capture device.
 *
 * @param capture Pointer to the PipeWire capture instance.
 */
void pipewire_capture_close(pipewire_capture_t* capture);

/**
 * @brief Get any pending sample rate change.
 *
 * @param capture Pointer to the PipeWire capture instance.
 * @param out_rate Pointer to double to store the pending sample rate.
 * @return true if a rate change is pending, false otherwise.
 */
bool pipewire_capture_get_pending_rate_change(pipewire_capture_t* capture,
                                              double* out_rate);

/**
 * @brief Check if pitch control is supported by the PipeWire capture backend.
 *
 * @param capture Pointer to the PipeWire capture instance.
 * @return true if supported, false otherwise.
 */
bool pipewire_capture_pitch_control_supported(pipewire_capture_t* capture);

/**
 * @brief Set the pitch multiplier for the PipeWire capture backend.
 *
 * @param capture Pointer to the PipeWire capture instance.
 * @param multiplier The pitch multiplier.
 */
void pipewire_capture_set_pitch(pipewire_capture_t* capture, double multiplier);

/**
 * @brief Wait for the PipeWire capture device to have data available.
 *
 * @param capture Pointer to the PipeWire capture instance.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if data is available, false on timeout or error.
 */
bool pipewire_capture_wait(pipewire_capture_t* capture, uint32_t timeout_ms);

/**
 * @brief Destroy the PipeWire capture backend instance.
 *
 * @param capture Pointer to the PipeWire capture instance to destroy.
 */
void pipewire_capture_destroy(pipewire_capture_t* capture);

// Playback backend factory & methods

/**
 * @brief Create a PipeWire playback backend instance.
 *
 * @param config Pointer to the playback device configuration.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The size of each audio chunk in frames.
 * @param params Pointer to processing parameters.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return Pointer to the created playback_backend_t instance, or NULL on
 * failure.
 */
playback_backend_t* pipewire_playback_create(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    processing_parameters_t* params, backend_error_t* err);

/**
 * @brief Open the PipeWire playback device.
 *
 * @param playback Pointer to the PipeWire playback instance.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool pipewire_playback_open(pipewire_playback_t* playback,
                            backend_error_t* err);

/**
 * @brief Write an audio chunk to the PipeWire playback device.
 *
 * @param playback Pointer to the PipeWire playback instance.
 * @param chunk Pointer to the audio chunk to write.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool pipewire_playback_write(pipewire_playback_t* playback,
                             const audio_chunk_t* chunk, backend_error_t* err);

/**
 * @brief Close the PipeWire playback device.
 *
 * @param playback Pointer to the PipeWire playback instance.
 */
void pipewire_playback_close(pipewire_playback_t* playback);

/**
 * @brief Get the current buffer level of the PipeWire playback backend.
 *
 * @param playback Pointer to the PipeWire playback instance.
 * @return The buffer level in samples.
 */
size_t pipewire_playback_get_buffer_level(pipewire_playback_t* playback);

/**
 * @brief Get any pending sample rate change.
 *
 * @param playback Pointer to the PipeWire playback instance.
 * @param out_rate Pointer to double to store the pending sample rate.
 * @return true if a rate change is pending, false otherwise.
 */
bool pipewire_playback_get_pending_rate_change(pipewire_playback_t* playback,
                                               double* out_rate);

/**
 * @brief Prefill the PipeWire playback buffer with silence.
 *
 * @param playback Pointer to the PipeWire playback instance.
 * @param frames Number of frames of silence to prefill.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool pipewire_playback_prefill_silence(pipewire_playback_t* playback,
                                       size_t frames, backend_error_t* err);

/**
 * @brief Check if PipeWire playback is currently paused.
 *
 * @param playback Pointer to the PipeWire playback instance.
 * @return true if paused, false otherwise.
 */
bool pipewire_playback_get_is_paused(pipewire_playback_t* playback);

/**
 * @brief Set the paused state of the PipeWire playback backend.
 *
 * @param playback Pointer to the PipeWire playback instance.
 * @param paused true to pause, false to resume.
 */
void pipewire_playback_set_is_paused(pipewire_playback_t* playback,
                                     bool paused);

/**
 * @brief Destroy the PipeWire playback backend instance.
 *
 * @param playback Pointer to the PipeWire playback instance to destroy.
 */
void pipewire_playback_destroy(pipewire_playback_t* playback);

#endif  // ENABLE_PIPEWIRE

#endif  // CLIB_BACKEND_PIPEWIRE_BACKEND_H
