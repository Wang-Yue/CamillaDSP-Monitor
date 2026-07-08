/**
 * @file core_audio_playback.h
 * @brief CoreAudio playback backend for macOS.
 *
 * Real-time discipline
 * --------------------
 * The render callback runs on a high-priority audio thread driven by
 * CoreAudio. It is absolutely forbidden to take locks, allocate, or
 * otherwise call into the Swift runtime in a way that could block. To
 * honour that:
 *   - sample rings are SPSC `SPSCAudioRingBuffer<Float>` instances —
 *     producer and consumer are wait-free, no `NSLock`.
 *   - the render callback writes directly into the AudioBufferList
 *     provided by CoreAudio, consuming from the pre-allocated SPSC rings.
 */

#ifndef CLIB_BACKEND_CORE_AUDIO_PLAYBACK_H
#define CLIB_BACKEND_CORE_AUDIO_PLAYBACK_H

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
 * @brief Opaque structure representing the CoreAudio playback backend.
 */
typedef struct core_audio_playback core_audio_playback_t;

/**
 * @brief Create a CoreAudio playback backend instance.
 *
 * @param config Pointer to the playback device configuration.
 * @param sample_rate The initial sample rate in Hz.
 * @param chunk_size The size of each audio chunk in frames.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return Pointer to the created playback_backend_t instance, or NULL on
 * failure.
 */
playback_backend_t* core_audio_playback_create(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    backend_error_t* err);

/**
 * @brief Open the CoreAudio playback device and initialize output AudioUnit.
 *
 * @param playback Pointer to the CoreAudio playback instance.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool core_audio_playback_open(core_audio_playback_t* playback,
                              backend_error_t* err);

/**
 * @brief Write an audio chunk into the playback ring buffers.
 *
 * @param playback Pointer to the CoreAudio playback instance.
 * @param chunk Pointer to the audio chunk to write.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool core_audio_playback_write(core_audio_playback_t* playback,
                               const audio_chunk_t* chunk,
                               backend_error_t* err);

/**
 * @brief Close the CoreAudio playback device and release HAL resources.
 *
 * @param playback Pointer to the CoreAudio playback instance.
 */
void core_audio_playback_close(core_audio_playback_t* playback);

/**
 * @brief Get the current buffer level in samples.
 *
 * @param playback Pointer to the CoreAudio playback instance.
 * @return The current buffer level in samples.
 */
size_t core_audio_playback_get_buffer_level(core_audio_playback_t* playback);

/**
 * @brief Get any pending sample rate change detected on the playback device.
 *
 * @param playback Pointer to the CoreAudio playback instance.
 * @param out_rate Pointer to double to store the pending sample rate.
 * @return true if a rate change is pending, false otherwise.
 */
bool core_audio_playback_get_pending_rate_change(
    core_audio_playback_t* playback, double* out_rate);

/**
 * @brief Push zero samples into the playback ring buffer before real audio
 * arrives.
 *
 * @param playback Pointer to the CoreAudio playback instance.
 * @param frames Number of frames of silence to prefill.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
bool core_audio_playback_prefill_silence(core_audio_playback_t* playback,
                                         size_t frames, backend_error_t* err);

/**
 * @brief Check if playback is currently paused.
 *
 * @param playback Pointer to the CoreAudio playback instance.
 * @return true if paused, false otherwise.
 */
bool core_audio_playback_get_is_paused(core_audio_playback_t* playback);

/**
 * @brief Set playback paused status.
 *
 * @param playback Pointer to the CoreAudio playback instance.
 * @param paused true to pause playback, false to resume.
 */
void core_audio_playback_set_is_paused(core_audio_playback_t* playback,
                                       bool paused);

/**
 * @brief Destroy and free the CoreAudio playback backend.
 *
 * @param playback Pointer to the CoreAudio playback instance.
 */
void core_audio_playback_destroy(core_audio_playback_t* playback);

#endif  // ENABLE_COREAUDIO

#endif  // CLIB_BACKEND_CORE_AUDIO_PLAYBACK_H
