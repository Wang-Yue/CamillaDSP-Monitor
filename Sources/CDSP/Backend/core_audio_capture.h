// CoreAudio capture backend for macOS
//
// Real-time discipline
// --------------------
// The render callback runs on a high-priority audio thread driven by
// CoreAudio. It is absolutely forbidden to take locks, allocate, or
// otherwise call into the Swift runtime in a way that could block. To
// honour that:
//   - sample rings are SPSC `SPSCAudioRingBuffer<Float>` instances —
//     producer and consumer are wait-free, no `NSLock`.
//   - the AudioBufferList plus its per-channel raw data buffers are
//     preallocated in `open()` and reused for the lifetime of the unit;
//     the render callback only fills the existing struct.

#ifndef CLIB_BACKEND_CORE_AUDIO_CAPTURE_H
#define CLIB_BACKEND_CORE_AUDIO_CAPTURE_H

#if defined(ENABLE_COREAUDIO)

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Audio/lock_free_ring_buffer.h"
#include "audio_backend.h"
#include "core_audio_device.h"

/**
 * @file core_audio_capture.h
 * @brief CoreAudio capture backend for macOS.
 *
 * This backend handles audio capture from CoreAudio devices.
 *
 * @note Real-time discipline:
 * The render callback runs on a high-priority audio thread driven by CoreAudio.
 * It is forbidden to take locks, allocate, or call functions that might block.
 * - Sample rings are SPSC (Single Producer Single Consumer) lock-free buffers.
 * - AudioBufferList and per-channel raw data buffers are preallocated in open()
 *   and reused.
 */

/**
 * @brief Opaque structure representing a CoreAudio capture backend instance.
 */
typedef struct core_audio_capture core_audio_capture_t;

/**
 * @brief Create a CoreAudio capture backend instance.
 *
 * @param config Configuration for the capture device.
 * @param sample_rate Target sample rate in Hz.
 * @param chunk_size Size of audio chunks to read.
 * @param err Pointer to backend error structure to report errors.
 * @return Pointer to the created capture_backend_t, or NULL on failure.
 */
capture_backend_t* core_audio_capture_create(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    backend_error_t* err);

/**
 * @brief Open the CoreAudio capture device.
 *
 * Initializes the AudioUnit and preallocates render buffers.
 *
 * @param capture Pointer to the CoreAudio capture instance.
 * @param err Pointer to backend error structure.
 * @return true if successful, false otherwise.
 */
bool core_audio_capture_open(core_audio_capture_t* capture,
                             backend_error_t* err);

/**
 * @brief Read audio frames from the capture ring buffers.
 *
 * Copies data into the provided audio chunk.
 *
 * @param capture Pointer to the CoreAudio capture instance.
 * @param frames Number of frames to read.
 * @param chunk Pointer to the audio chunk structure to store read data.
 * @param err Pointer to backend error structure.
 * @return true if successful, false otherwise.
 */
bool core_audio_capture_read(core_audio_capture_t* capture, size_t frames,
                             audio_chunk_t* chunk, backend_error_t* err);

/**
 * @brief Close the CoreAudio capture device.
 *
 * Releases HAL resources.
 *
 * @param capture Pointer to the CoreAudio capture instance.
 */
void core_audio_capture_close(core_audio_capture_t* capture);

/**
 * @brief Get any pending sample rate change detected on the capture device.
 *
 * @param capture Pointer to the CoreAudio capture instance.
 * @param out_rate Pointer to double to receive the new rate if pending.
 * @return true if there is a pending rate change, false otherwise.
 */
bool core_audio_capture_get_pending_rate_change(core_audio_capture_t* capture,
                                                double* out_rate);

/**
 * @brief Check if clock-pitch control is supported on the capture device.
 *
 * @param capture Pointer to the CoreAudio capture instance.
 * @return true if supported, false otherwise.
 */
bool core_audio_capture_pitch_control_supported(core_audio_capture_t* capture);

/**
 * @brief Apply a clock-pitch correction to the capture device.
 *
 * @param capture Pointer to the CoreAudio capture instance.
 * @param multiplier Pitch multiplier.
 */
void core_audio_capture_set_pitch(core_audio_capture_t* capture,
                                  double multiplier);

/**
 * @brief Wait for new samples to become available.
 *
 * @param capture Pointer to the CoreAudio capture instance.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if data is available, false if timed out or error occurred.
 */
bool core_audio_capture_wait(core_audio_capture_t* capture,
                             uint32_t timeout_ms);

/**
 * @brief Destroy and free the CoreAudio capture backend.
 *
 * @param capture Pointer to the CoreAudio capture instance to destroy.
 */
void core_audio_capture_destroy(core_audio_capture_t* capture);

#endif  // ENABLE_COREAUDIO

#endif  // CLIB_BACKEND_CORE_AUDIO_CAPTURE_H
