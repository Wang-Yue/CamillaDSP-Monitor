#ifndef CLIB_BACKEND_GENERATOR_CAPTURE_H
#define CLIB_BACKEND_GENERATOR_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct generator_capture generator_capture_t;

typedef struct processing_parameters processing_parameters_t;

capture_backend_t* generator_capture_create(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    processing_parameters_t* params, backend_error_t* err);
bool generator_capture_open(generator_capture_t* capture, backend_error_t* err);
bool generator_capture_read(generator_capture_t* capture, size_t frames,
                            audio_chunk_t* chunk, backend_error_t* err);
void generator_capture_close(generator_capture_t* capture);
bool generator_capture_get_pending_rate_change(generator_capture_t* capture,
                                               double* out_rate);
bool generator_capture_pitch_control_supported(generator_capture_t* capture);
void generator_capture_set_pitch(generator_capture_t* capture,
                                 double multiplier);
bool generator_capture_wait(generator_capture_t* capture, uint32_t timeout_ms);
void generator_capture_destroy(generator_capture_t* capture);

#ifdef __cplusplus
}
#endif

#endif  // CLIB_BACKEND_GENERATOR_CAPTURE_H
