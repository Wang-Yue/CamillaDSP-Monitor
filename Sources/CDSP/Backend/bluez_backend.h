#ifndef CLIB_BACKEND_BLUEZ_BACKEND_H
#define CLIB_BACKEND_BLUEZ_BACKEND_H

#if defined(ENABLE_BLUEZ)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_backend.h"

typedef struct bluez_capture bluez_capture_t;
typedef struct processing_parameters processing_parameters_t;

// Capture backend factory & methods
capture_backend_t* bluez_capture_create(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    processing_parameters_t* params, backend_error_t* err);
bool bluez_capture_open(bluez_capture_t* capture, backend_error_t* err);
bool bluez_capture_read(bluez_capture_t* capture, size_t frames,
                        audio_chunk_t* chunk, backend_error_t* err);
void bluez_capture_close(bluez_capture_t* capture);
bool bluez_capture_get_pending_rate_change(bluez_capture_t* capture,
                                           double* out_rate);
bool bluez_capture_pitch_control_supported(bluez_capture_t* capture);
void bluez_capture_set_pitch(bluez_capture_t* capture, double multiplier);
bool bluez_capture_wait(bluez_capture_t* capture, uint32_t timeout_ms);
void bluez_capture_destroy(bluez_capture_t* capture);

#endif  // ENABLE_BLUEZ

#endif  // CLIB_BACKEND_BLUEZ_BACKEND_H
