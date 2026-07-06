#ifndef CLIB_BACKEND_ALSA_PLAYBACK_H
#define CLIB_BACKEND_ALSA_PLAYBACK_H

#ifndef __APPLE__

#include "audio_backend.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct alsa_playback alsa_playback_t;

playback_backend_t* alsa_playback_create(const playback_device_config_t* config, int sample_rate, int chunk_size, backend_error_t* err);
bool alsa_playback_open(alsa_playback_t* playback, backend_error_t* err);
bool alsa_playback_write(alsa_playback_t* playback, const audio_chunk_t* chunk, backend_error_t* err);
void alsa_playback_close(alsa_playback_t* playback);
size_t alsa_playback_get_buffer_level(alsa_playback_t* playback);
bool alsa_playback_get_pending_rate_change(alsa_playback_t* playback, double* out_rate);
bool alsa_playback_prefill_silence(alsa_playback_t* playback, size_t frames, backend_error_t* err);
bool alsa_playback_get_is_paused(alsa_playback_t* playback);
void alsa_playback_set_is_paused(alsa_playback_t* playback, bool paused);
void alsa_playback_destroy(alsa_playback_t* playback);

#ifdef __cplusplus
}
#endif

#endif // !__APPLE__

#endif // CLIB_BACKEND_ALSA_PLAYBACK_H
