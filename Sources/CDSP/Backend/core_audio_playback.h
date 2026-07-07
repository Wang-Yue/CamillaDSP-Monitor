// CoreAudio playback backend for macOS
//
// Real-time discipline
// --------------------
// The render callback runs on a high-priority audio thread driven by
// CoreAudio. It is absolutely forbidden to take locks, allocate, or
// otherwise call into the Swift runtime in a way that could block. To
// honour that:
//   - sample rings are SPSC `SPSCAudioRingBuffer<Float>` instances —
//     producer and consumer are wait-free, no `NSLock`.
//   - the render callback writes directly into the AudioBufferList
//     provided by CoreAudio, consuming from the pre-allocated SPSC rings.

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

typedef struct core_audio_playback core_audio_playback_t;

/// Create a CoreAudio playback backend instance.
playback_backend_t* core_audio_playback_create(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    backend_error_t* err);
/// Open the CoreAudio playback device and initialize output AudioUnit.
bool core_audio_playback_open(core_audio_playback_t* playback,
                              backend_error_t* err);
/// Write an audio chunk into the playback ring buffers.
bool core_audio_playback_write(core_audio_playback_t* playback,
                               const audio_chunk_t* chunk,
                               backend_error_t* err);
/// Close the CoreAudio playback device and release HAL resources.
void core_audio_playback_close(core_audio_playback_t* playback);
/// Get the current buffer level in samples.
size_t core_audio_playback_get_buffer_level(core_audio_playback_t* playback);
/// Get any pending sample rate change detected on the playback device.
bool core_audio_playback_get_pending_rate_change(
    core_audio_playback_t* playback, double* out_rate);
/// Push zero samples into the playback ring buffer before real audio arrives.
bool core_audio_playback_prefill_silence(core_audio_playback_t* playback,
                                         size_t frames, backend_error_t* err);
/// Check if playback is currently paused.
bool core_audio_playback_get_is_paused(core_audio_playback_t* playback);
/// Set playback paused status.
void core_audio_playback_set_is_paused(core_audio_playback_t* playback,
                                       bool paused);
/// Destroy and free the CoreAudio playback backend.
void core_audio_playback_destroy(core_audio_playback_t* playback);

#endif  // ENABLE_COREAUDIO

#endif  // CLIB_BACKEND_CORE_AUDIO_PLAYBACK_H
