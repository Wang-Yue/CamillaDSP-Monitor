#ifndef CLIB_BACKEND_FILE_BACKEND_H
#define CLIB_BACKEND_FILE_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_backend.h"

typedef struct file_capture file_capture_t;
typedef struct file_playback file_playback_t;

typedef struct processing_parameters processing_parameters_t;

// Capture backend factory & methods
capture_backend_t* file_capture_create(const capture_device_config_t* config,
                                       int sample_rate, int chunk_size,
                                       processing_parameters_t* params,
                                       backend_error_t* err);
bool file_capture_open(file_capture_t* capture, backend_error_t* err);
bool file_capture_read(file_capture_t* capture, size_t frames,
                       audio_chunk_t* chunk, backend_error_t* err);
void file_capture_close(file_capture_t* capture);
bool file_capture_get_pending_rate_change(file_capture_t* capture,
                                          double* out_rate);
bool file_capture_pitch_control_supported(file_capture_t* capture);
void file_capture_set_pitch(file_capture_t* capture, double multiplier);
bool file_capture_wait(file_capture_t* capture, uint32_t timeout_ms);
void file_capture_set_is_paused(file_capture_t* capture, bool paused);
void file_capture_destroy(file_capture_t* capture);

// Playback backend factory & methods
playback_backend_t* file_playback_create(const playback_device_config_t* config,
                                         int sample_rate, int chunk_size,
                                         processing_parameters_t* params,
                                         backend_error_t* err);
bool file_playback_open(file_playback_t* playback, backend_error_t* err);
bool file_playback_write(file_playback_t* playback, const audio_chunk_t* chunk,
                         backend_error_t* err);
void file_playback_close(file_playback_t* playback);
size_t file_playback_get_buffer_level(file_playback_t* playback);
bool file_playback_get_pending_rate_change(file_playback_t* playback,
                                           double* out_rate);
bool file_playback_prefill_silence(file_playback_t* playback, size_t frames,
                                   backend_error_t* err);
bool file_playback_get_is_paused(file_playback_t* playback);
void file_playback_set_is_paused(file_playback_t* playback, bool paused);
void file_playback_destroy(file_playback_t* playback);

#endif  // CLIB_BACKEND_FILE_BACKEND_H
