#ifndef CLIB_BACKEND_PULSE_BACKEND_H
#define CLIB_BACKEND_PULSE_BACKEND_H

#if defined(__linux__)

#include "audio_backend.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pulse_capture pulse_capture_t;
typedef struct pulse_playback pulse_playback_t;

typedef struct processing_parameters processing_parameters_t;

// Capture backend factory & methods
capture_backend_t* pulse_capture_create(const capture_device_config_t* config, int sample_rate, int chunk_size, processing_parameters_t* params, backend_error_t* err);
bool pulse_capture_open(pulse_capture_t* capture, backend_error_t* err);
bool pulse_capture_read(pulse_capture_t* capture, size_t frames, audio_chunk_t* chunk, backend_error_t* err);
void pulse_capture_close(pulse_capture_t* capture);
bool pulse_capture_get_pending_rate_change(pulse_capture_t* capture, double* out_rate);
bool pulse_capture_pitch_control_supported(pulse_capture_t* capture);
void pulse_capture_set_pitch(pulse_capture_t* capture, double multiplier);
bool pulse_capture_wait(pulse_capture_t* capture, uint32_t timeout_ms);
void pulse_capture_destroy(pulse_capture_t* capture);

// Playback backend factory & methods
playback_backend_t* pulse_playback_create(const playback_device_config_t* config, int sample_rate, int chunk_size, processing_parameters_t* params, backend_error_t* err);
bool pulse_playback_open(pulse_playback_t* playback, backend_error_t* err);
bool pulse_playback_write(pulse_playback_t* playback, const audio_chunk_t* chunk, backend_error_t* err);
void pulse_playback_close(pulse_playback_t* playback);
size_t pulse_playback_get_buffer_level(pulse_playback_t* playback);
bool pulse_playback_get_pending_rate_change(pulse_playback_t* playback, double* out_rate);
bool pulse_playback_prefill_silence(pulse_playback_t* playback, size_t frames, backend_error_t* err);
bool pulse_playback_get_is_paused(pulse_playback_t* playback);
void pulse_playback_set_is_paused(pulse_playback_t* playback, bool paused);
void pulse_playback_destroy(pulse_playback_t* playback);

#ifdef __cplusplus
}
#endif

#endif // __linux__

#endif // CLIB_BACKEND_PULSE_BACKEND_H
