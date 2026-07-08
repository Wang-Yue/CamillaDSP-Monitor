#ifndef CLIB_BACKEND_BLUEZ_BACKEND_H
#define CLIB_BACKEND_BLUEZ_BACKEND_H

#if defined(ENABLE_BLUEZ)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_backend.h"

/**
 * @file bluez_backend.h
 * @brief BlueZ audio capture backend.
 *
 * Provides functions to interact with BlueZ (Bluetooth) audio capture devices.
 */

/**
 * @brief Opaque structure representing a BlueZ capture backend instance.
 */
typedef struct bluez_capture bluez_capture_t;

/**
 * @brief Opaque structure representing audio processing parameters.
 */
typedef struct processing_parameters processing_parameters_t;

/**
 * @brief Create a BlueZ capture backend instance.
 *
 * @param config Configuration for the capture device.
 * @param sample_rate Target sample rate in Hz.
 * @param chunk_size Size of audio chunks to read.
 * @param params Processing parameters.
 * @param err Pointer to backend error structure to report errors.
 * @return Pointer to the created capture_backend_t, or NULL on failure.
 */
capture_backend_t* bluez_capture_create(const capture_device_config_t* config,
                                        int sample_rate, int chunk_size,
                                        processing_parameters_t* params,
                                        backend_error_t* err);

/**
 * @brief Open the BlueZ capture device.
 *
 * @param capture Pointer to the BlueZ capture instance.
 * @param err Pointer to backend error structure.
 * @return true if successful, false otherwise.
 */
bool bluez_capture_open(bluez_capture_t* capture, backend_error_t* err);

/**
 * @brief Read audio frames from the BlueZ capture device.
 *
 * @param capture Pointer to the BlueZ capture instance.
 * @param frames Number of frames to read.
 * @param chunk Pointer to the audio chunk structure to store read data.
 * @param err Pointer to backend error structure.
 * @return true if successful, false otherwise.
 */
bool bluez_capture_read(bluez_capture_t* capture, size_t frames,
                        audio_chunk_t* chunk, backend_error_t* err);

/**
 * @brief Close the BlueZ capture device.
 *
 * @param capture Pointer to the BlueZ capture instance.
 */
void bluez_capture_close(bluez_capture_t* capture);

/**
 * @brief Check if there is a pending rate change request.
 *
 * @param capture Pointer to the BlueZ capture instance.
 * @param out_rate Pointer to double to receive the new rate if pending.
 * @return true if there is a pending rate change, false otherwise.
 */
bool bluez_capture_get_pending_rate_change(bluez_capture_t* capture,
                                           double* out_rate);

/**
 * @brief Check if pitch control is supported by this backend.
 *
 * @param capture Pointer to the BlueZ capture instance.
 * @return true if supported, false otherwise.
 */
bool bluez_capture_pitch_control_supported(bluez_capture_t* capture);

/**
 * @brief Set the pitch multiplier for the capture device.
 *
 * @param capture Pointer to the BlueZ capture instance.
 * @param multiplier Pitch multiplier (e.g. 1.0 for normal pitch).
 */
void bluez_capture_set_pitch(bluez_capture_t* capture, double multiplier);

/**
 * @brief Wait for audio data to be available for reading.
 *
 * @param capture Pointer to the BlueZ capture instance.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if data is available, false if timed out or error occurred.
 */
bool bluez_capture_wait(bluez_capture_t* capture, uint32_t timeout_ms);

/**
 * @brief Destroy the BlueZ capture backend instance.
 *
 * Releases all resources associated with the instance.
 *
 * @param capture Pointer to the BlueZ capture instance to destroy.
 */
void bluez_capture_destroy(bluez_capture_t* capture);

#endif  // ENABLE_BLUEZ

#endif  // CLIB_BACKEND_BLUEZ_BACKEND_H
