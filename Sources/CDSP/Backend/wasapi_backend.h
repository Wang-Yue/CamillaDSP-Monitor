#ifndef CLIB_BACKEND_WASAPI_BACKEND_H
#define CLIB_BACKEND_WASAPI_BACKEND_H

#if defined(_WIN32)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wasapi_capture wasapi_capture_t;
typedef struct wasapi_playback wasapi_playback_t;

typedef struct processing_parameters processing_parameters_t;

// Capture backend factory & methods
capture_backend_t* wasapi_capture_create(const capture_device_config_t* config,
                                         int sample_rate, int chunk_size,
                                         processing_parameters_t* params,
                                         backend_error_t* err);
bool wasapi_capture_open(wasapi_capture_t* capture, backend_error_t* err);
bool wasapi_capture_read(wasapi_capture_t* capture, size_t frames,
                         audio_chunk_t* chunk, backend_error_t* err);
void wasapi_capture_close(wasapi_capture_t* capture);
bool wasapi_capture_get_pending_rate_change(wasapi_capture_t* capture,
                                            double* out_rate);
bool wasapi_capture_pitch_control_supported(wasapi_capture_t* capture);
void wasapi_capture_set_pitch(wasapi_capture_t* capture, double multiplier);
bool wasapi_capture_wait(wasapi_capture_t* capture, uint32_t timeout_ms);
void wasapi_capture_destroy(wasapi_capture_t* capture);

// Playback backend factory & methods
playback_backend_t* wasapi_playback_create(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    processing_parameters_t* params, backend_error_t* err);
bool wasapi_playback_open(wasapi_playback_t* playback, backend_error_t* err);
bool wasapi_playback_write(wasapi_playback_t* playback,
                           const audio_chunk_t* chunk, backend_error_t* err);
void wasapi_playback_close(wasapi_playback_t* playback);
size_t wasapi_playback_get_buffer_level(wasapi_playback_t* playback);
bool wasapi_playback_get_pending_rate_change(wasapi_playback_t* playback,
                                             double* out_rate);
bool wasapi_playback_prefill_silence(wasapi_playback_t* playback, size_t frames,
                                     backend_error_t* err);
bool wasapi_playback_get_is_paused(wasapi_playback_t* playback);
void wasapi_playback_set_is_paused(wasapi_playback_t* playback, bool paused);
void wasapi_playback_destroy(wasapi_playback_t* playback);

#ifdef __cplusplus
}
#endif

#endif  // _WIN32

#endif  // CLIB_BACKEND_WASAPI_BACKEND_H
