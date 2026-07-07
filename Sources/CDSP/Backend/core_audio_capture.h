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

#ifdef __APPLE__

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Audio/lock_free_ring_buffer.h"
#include "audio_backend.h"
#include "core_audio_device.h"

typedef struct core_audio_capture core_audio_capture_t;

/// Create a CoreAudio capture backend instance.
capture_backend_t* core_audio_capture_create(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    backend_error_t* err);
/// Open the CoreAudio capture device and initialize the AudioUnit and render
/// buffers.
bool core_audio_capture_open(core_audio_capture_t* capture,
                             backend_error_t* err);
/// Read a chunk of audio from the capture ring buffers into the provided audio
/// chunk.
bool core_audio_capture_read(core_audio_capture_t* capture, size_t frames,
                             audio_chunk_t* chunk, backend_error_t* err);
/// Close the CoreAudio capture device and release HAL resources.
void core_audio_capture_close(core_audio_capture_t* capture);
/// Get any pending sample rate change detected on the capture device.
bool core_audio_capture_get_pending_rate_change(core_audio_capture_t* capture,
                                                double* out_rate);
/// Check if clock-pitch control is supported on the capture device.
bool core_audio_capture_pitch_control_supported(core_audio_capture_t* capture);
/// Apply a clock-pitch correction to the capture device.
void core_audio_capture_set_pitch(core_audio_capture_t* capture,
                                  double multiplier);
/// Wait for new samples to become available, up to the given timeout.
bool core_audio_capture_wait(core_audio_capture_t* capture,
                             uint32_t timeout_ms);
/// Destroy and free the CoreAudio capture backend.
void core_audio_capture_destroy(core_audio_capture_t* capture);

#endif  // __APPLE__

#endif  // CLIB_BACKEND_CORE_AUDIO_CAPTURE_H
