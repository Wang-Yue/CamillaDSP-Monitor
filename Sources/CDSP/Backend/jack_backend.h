#ifndef CLIB_BACKEND_JACK_BACKEND_H
#define CLIB_BACKEND_JACK_BACKEND_H

#if defined(ENABLE_JACK)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_backend.h"

typedef struct jack_capture jack_capture_t;
typedef struct jack_playback jack_playback_t;

typedef struct processing_parameters processing_parameters_t;

// Capture backend factory & methods
capture_backend_t* jack_capture_create(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    processing_parameters_t* params, backend_error_t* err);
bool jack_capture_open(jack_capture_t* capture, backend_error_t* err);
bool jack_capture_read(jack_capture_t* capture, size_t frames,
                       audio_chunk_t* chunk, backend_error_t* err);
void jack_capture_close(jack_capture_t* capture);
bool jack_capture_get_pending_rate_change(jack_capture_t* capture,
                                          double* out_rate);
bool jack_capture_pitch_control_supported(jack_capture_t* capture);
void jack_capture_set_pitch(jack_capture_t* capture, double multiplier);
bool jack_capture_wait(jack_capture_t* capture, uint32_t timeout_ms);
void jack_capture_destroy(jack_capture_t* capture);

// Playback backend factory & methods
playback_backend_t* jack_playback_create(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    processing_parameters_t* params, backend_error_t* err);
bool jack_playback_open(jack_playback_t* playback,
                        backend_error_t* err);
bool jack_playback_write(jack_playback_t* playback,
                         const audio_chunk_t* chunk, backend_error_t* err);
void jack_playback_close(jack_playback_t* playback);
size_t jack_playback_get_buffer_level(jack_playback_t* playback);
bool jack_playback_get_pending_rate_change(jack_playback_t* playback,
                                           double* out_rate);
bool jack_playback_prefill_silence(jack_playback_t* playback,
                                   size_t frames, backend_error_t* err);
bool jack_playback_get_is_paused(jack_playback_t* playback);
void jack_playback_set_is_paused(jack_playback_t* playback,
                                 bool paused);
void jack_playback_destroy(jack_playback_t* playback);

#endif  // ENABLE_JACK

#endif  // CLIB_BACKEND_JACK_BACKEND_H
