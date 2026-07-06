#ifndef CLIB_BACKEND_ALSA_CAPTURE_H
#define CLIB_BACKEND_ALSA_CAPTURE_H

#ifndef __APPLE__

#include "audio_backend.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct alsa_capture alsa_capture_t;

typedef struct processing_parameters processing_parameters_t;
capture_backend_t* alsa_capture_create(const capture_device_config_t* config, int sample_rate, int chunk_size, processing_parameters_t* params, backend_error_t* err);
bool alsa_capture_open(alsa_capture_t* capture, backend_error_t* err);
bool alsa_capture_read(alsa_capture_t* capture, size_t frames, audio_chunk_t* chunk, backend_error_t* err);
void alsa_capture_close(alsa_capture_t* capture);
bool alsa_capture_get_pending_rate_change(alsa_capture_t* capture, double* out_rate);
bool alsa_capture_pitch_control_supported(alsa_capture_t* capture);
void alsa_capture_set_pitch(alsa_capture_t* capture, double multiplier);
bool alsa_capture_wait(alsa_capture_t* capture, uint32_t timeout_ms);
void alsa_capture_destroy(alsa_capture_t* capture);

#ifdef __cplusplus
}
#endif

#endif // !__APPLE__

#endif // CLIB_BACKEND_ALSA_CAPTURE_H
