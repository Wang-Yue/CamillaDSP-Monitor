/**
 * @file generator_capture.h
 * @brief Signal generator-based audio capture backend.
 */

#ifndef CLIB_BACKEND_GENERATOR_CAPTURE_H
#define CLIB_BACKEND_GENERATOR_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_backend.h"

/**
 * @brief Opaque structure representing the generator capture backend.
 */
typedef struct generator_capture generator_capture_t;

/**
 * @brief Opaque structure representing processing parameters.
 */
typedef struct processing_parameters processing_parameters_t;

/**
 * @brief Create a generator capture backend instance.
 *
 * @param config Pointer to the capture device configuration.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The size of each audio chunk in frames.
 * @param params Pointer to processing parameters.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return Pointer to the created capture_backend_t instance, or NULL on
 * failure.
 */
capture_backend_t* generator_capture_create(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    processing_parameters_t* params, backend_error_t* err);

/**
 * @brief Open the generator capture device.
 *
 * @param capture Pointer to the generator capture instance.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool generator_capture_open(generator_capture_t* capture, backend_error_t* err);

/**
 * @brief Read audio frames from the generator capture device.
 *
 * @param capture Pointer to the generator capture instance.
 * @param frames Number of frames to read.
 * @param chunk Pointer to the audio chunk to fill.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool generator_capture_read(generator_capture_t* capture, size_t frames,
                            audio_chunk_t* chunk, backend_error_t* err);

/**
 * @brief Close the generator capture device.
 *
 * @param capture Pointer to the generator capture instance.
 */
void generator_capture_close(generator_capture_t* capture);

/**
 * @brief Get any pending sample rate change.
 *
 * @param capture Pointer to the generator capture instance.
 * @param out_rate Pointer to double to store the pending sample rate.
 * @return true if a rate change is pending, false otherwise.
 */
bool generator_capture_get_pending_rate_change(generator_capture_t* capture,
                                               double* out_rate);

/**
 * @brief Check if pitch control is supported by the generator capture backend.
 *
 * @param capture Pointer to the generator capture instance.
 * @return true if supported, false otherwise.
 */
bool generator_capture_pitch_control_supported(generator_capture_t* capture);

/**
 * @brief Set the pitch multiplier for the generator capture backend.
 *
 * @param capture Pointer to the generator capture instance.
 * @param multiplier The pitch multiplier.
 */
void generator_capture_set_pitch(generator_capture_t* capture,
                                 double multiplier);

/**
 * @brief Wait for the generator capture device to have data available.
 *
 * @param capture Pointer to the generator capture instance.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if data is available, false on timeout or error.
 */
bool generator_capture_wait(generator_capture_t* capture, uint32_t timeout_ms);

/**
 * @brief Set the paused state of the generator capture backend.
 *
 * @param capture Pointer to the generator capture instance.
 * @param paused true to pause, false to resume.
 */
void generator_capture_set_is_paused(generator_capture_t* capture, bool paused);

/**
 * @brief Destroy the generator capture backend instance.
 *
 * @param capture Pointer to the generator capture instance to destroy.
 */
void generator_capture_destroy(generator_capture_t* capture);

#endif  // CLIB_BACKEND_GENERATOR_CAPTURE_H
