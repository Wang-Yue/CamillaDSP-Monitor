#ifndef CLIB_BACKEND_PIPEWIRE_BACKEND_H
#define CLIB_BACKEND_PIPEWIRE_BACKEND_H

#if defined(__linux__)

#include "audio_backend.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pipewire_capture pipewire_capture_t;
typedef struct pipewire_playback pipewire_playback_t;

typedef struct processing_parameters processing_parameters_t;

// Capture backend factory & methods
capture_backend_t* pipewire_capture_create(const capture_device_config_t* config, int sample_rate, int chunk_size, processing_parameters_t* params, backend_error_t* err);
bool pipewire_capture_open(pipewire_capture_t* capture, backend_error_t* err);
bool pipewire_capture_read(pipewire_capture_t* capture, size_t frames, audio_chunk_t* chunk, backend_error_t* err);
void pipewire_capture_close(pipewire_capture_t* capture);
bool pipewire_capture_get_pending_rate_change(pipewire_capture_t* capture, double* out_rate);
bool pipewire_capture_pitch_control_supported(pipewire_capture_t* capture);
void pipewire_capture_set_pitch(pipewire_capture_t* capture, double multiplier);
bool pipewire_capture_wait(pipewire_capture_t* capture, uint32_t timeout_ms);
void pipewire_capture_destroy(pipewire_capture_t* capture);

// Playback backend factory & methods
playback_backend_t* pipewire_playback_create(const playback_device_config_t* config, int sample_rate, int chunk_size, processing_parameters_t* params, backend_error_t* err);
bool pipewire_playback_open(pipewire_playback_t* playback, backend_error_t* err);
bool pipewire_playback_write(pipewire_playback_t* playback, const audio_chunk_t* chunk, backend_error_t* err);
void pipewire_playback_close(pipewire_playback_t* playback);
size_t pipewire_playback_get_buffer_level(pipewire_playback_t* playback);
bool pipewire_playback_get_pending_rate_change(pipewire_playback_t* playback, double* out_rate);
bool pipewire_playback_prefill_silence(pipewire_playback_t* playback, size_t frames, backend_error_t* err);
bool pipewire_playback_get_is_paused(pipewire_playback_t* playback);
void pipewire_playback_set_is_paused(pipewire_playback_t* playback, bool paused);
void pipewire_playback_destroy(pipewire_playback_t* playback);

#ifdef __cplusplus
}
#endif

#endif // __linux__

#endif // CLIB_BACKEND_PIPEWIRE_BACKEND_H
