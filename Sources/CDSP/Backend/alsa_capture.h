/**
 * @file alsa_capture.h
 * @brief ALSA capture backend implementation.
 *
 * This file defines the ALSA capture backend, which implements the
 * `capture_backend_t` interface for capturing audio from an ALSA device.
 * These functions are only available if `ENABLE_ALSA` is defined.
 */

#ifndef CLIB_BACKEND_ALSA_CAPTURE_H
#define CLIB_BACKEND_ALSA_CAPTURE_H

#if defined(ENABLE_ALSA)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_backend.h"

/**
 * @brief Opaque structure representing the ALSA capture backend instance.
 */
typedef struct alsa_capture alsa_capture_t;

typedef struct processing_parameters processing_parameters_t;

/**
 * @brief Creates a new ALSA capture backend instance.
 *
 * This function instantiates the ALSA capture backend, configuring it with the
 * provided device config, sample rate, and chunk size. It returns the generic
 * `capture_backend_t` interface.
 *
 * @param config Pointer to the capture device configuration.
 * @param sample_rate The target sample rate.
 * @param chunk_size The target chunk size (number of frames per read).
 * @param params Pointer to the processing parameters for telemetry updates.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return Pointer to the generic capture_backend_t interface, or NULL on
 * failure.
 */
capture_backend_t* alsa_capture_create(const capture_device_config_t* config,
                                       int sample_rate, int chunk_size,
                                       processing_parameters_t* params,
                                       backend_error_t* err);

/**
 * @brief Opens the ALSA capture device.
 *
 * @param capture Pointer to the ALSA capture instance.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return True on success, false otherwise.
 */
bool alsa_capture_open(alsa_capture_t* capture, backend_error_t* err);

/**
 * @brief Reads audio frames from the ALSA capture device.
 *
 * @param capture Pointer to the ALSA capture instance.
 * @param frames The number of frames to read.
 * @param chunk Pointer to the audio chunk to store the read samples.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return True on success, false otherwise.
 */
bool alsa_capture_read(alsa_capture_t* capture, size_t frames,
                       audio_chunk_t* chunk, backend_error_t* err);

/**
 * @brief Closes the ALSA capture device.
 *
 * @param capture Pointer to the ALSA capture instance.
 */
void alsa_capture_close(alsa_capture_t* capture);

/**
 * @brief Checks if there is a pending sample rate change detected by the
 * device.
 *
 * Some devices (e.g., USB interfaces) can report if the incoming sample rate
 * has changed.
 *
 * @param capture Pointer to the ALSA capture instance.
 * @param out_rate Pointer to store the new sample rate if a change is pending.
 * @return True if a rate change is pending, false otherwise.
 */
bool alsa_capture_get_pending_rate_change(alsa_capture_t* capture,
                                          double* out_rate);

/**
 * @brief Checks if pitch control (resampling adjust) is supported by the
 * backend.
 *
 * @param capture Pointer to the ALSA capture instance.
 * @return True if supported, false otherwise.
 */
bool alsa_capture_pitch_control_supported(alsa_capture_t* capture);

/**
 * @brief Sets the pitch multiplier (resampling adjustment factor) for the
 * capture device.
 *
 * Used for drift compensation.
 *
 * @param capture Pointer to the ALSA capture instance.
 * @param multiplier The pitch multiplier factor.
 */
void alsa_capture_set_pitch(alsa_capture_t* capture, double multiplier);

/**
 * @brief Waits for the ALSA capture device to have data available.
 *
 * @param capture Pointer to the ALSA capture instance.
 * @param timeout_ms The timeout in milliseconds.
 * @return True if data is available, false if timeout or error.
 */
bool alsa_capture_wait(alsa_capture_t* capture, uint32_t timeout_ms);

/**
 * @brief Destroys the ALSA capture instance and frees associated resources.
 *
 * @param capture Pointer to the ALSA capture instance.
 */
void alsa_capture_destroy(alsa_capture_t* capture);

#endif  // ENABLE_ALSA

#endif  // CLIB_BACKEND_ALSA_CAPTURE_H
