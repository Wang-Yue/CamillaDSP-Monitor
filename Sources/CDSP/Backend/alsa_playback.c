#ifndef __APPLE__
#define _GNU_SOURCE

#include <time.h>
#include <alloca.h>
#include <string.h>
#include "alsa_playback.h"
#include <alsa/asoundlib.h>
#include <stdlib.h>
#include <string.h>

struct alsa_playback {
    char device_name[256];
    int sample_rate;
    int channels;
    int chunk_size;

    bool has_format;
    alsa_sample_format_t requested_format;
    processing_parameters_t* params;

    snd_pcm_t* pcm;
    snd_pcm_format_t format;
    bool paused;

    void* interleaved_buf;
    size_t interleaved_buf_size;
};

static bool vtable_open(void* ctx, backend_error_t* err) {
    return alsa_playback_open((alsa_playback_t*)ctx, err);
}

static bool vtable_write(void* ctx, const audio_chunk_t* chunk, backend_error_t* err) {
    return alsa_playback_write((alsa_playback_t*)ctx, chunk, err);
}

static void vtable_close(void* ctx) {
    alsa_playback_close((alsa_playback_t*)ctx);
}

static size_t vtable_get_buffer_level(void* ctx) {
    return alsa_playback_get_buffer_level((alsa_playback_t*)ctx);
}

static bool vtable_get_rate(void* ctx, double* out_rate) {
    return alsa_playback_get_pending_rate_change((alsa_playback_t*)ctx, out_rate);
}

static bool vtable_prefill(void* ctx, size_t frames, backend_error_t* err) {
    return alsa_playback_prefill_silence((alsa_playback_t*)ctx, frames, err);
}

static bool vtable_get_paused(void* ctx) {
    return alsa_playback_get_is_paused((alsa_playback_t*)ctx);
}

static void vtable_set_paused(void* ctx, bool paused) {
    alsa_playback_set_is_paused((alsa_playback_t*)ctx, paused);
}

static void vtable_destroy(void* ctx) {
    alsa_playback_destroy((alsa_playback_t*)ctx);
}

static const playback_backend_vtable_t ALSA_PLAYBACK_VTABLE = {
    .open = vtable_open,
    .write = vtable_write,
    .close = vtable_close,
    .get_buffer_level = vtable_get_buffer_level,
    .get_pending_rate_change = vtable_get_rate,
    .prefill_silence = vtable_prefill,
    .get_is_paused = vtable_get_paused,
    .set_is_paused = vtable_set_paused,
    .destroy = vtable_destroy
};

playback_backend_t* alsa_playback_create(const playback_device_config_t* config, int sample_rate, int chunk_size, processing_parameters_t* params, backend_error_t* err) {
    (void)err;
    alsa_playback_t* playback = (alsa_playback_t*)calloc(1, sizeof(alsa_playback_t));
    if (!playback) return NULL;

    // Clean up name
    char clean_name[256];
    snprintf(clean_name, sizeof(clean_name), "%s", config->device[0] ? config->device : "default");
    char* space = strchr(clean_name, ' ');
    if (space) *space = '\0';
    snprintf(playback->device_name, sizeof(playback->device_name), "%s", clean_name);

    playback->sample_rate = sample_rate;
    playback->channels = config->channels;
    playback->chunk_size = chunk_size;

    playback->has_format = config->has_format;
    playback->requested_format = config->format;
    playback->params = params;

    playback_backend_t* backend = (playback_backend_t*)calloc(1, sizeof(playback_backend_t));
    if (!backend) {
        free(playback);
        return NULL;
    }
    backend->ctx = playback;
    backend->vtable = &ALSA_PLAYBACK_VTABLE;
    return backend;
}

bool alsa_playback_open(alsa_playback_t* playback, backend_error_t* err) {
    int rc;
    rc = snd_pcm_open(&playback->pcm, playback->device_name, SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, snd_strerror(rc));
        return false;
    }

    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);
    rc = snd_pcm_hw_params_any(playback->pcm, params);
    if (rc < 0) {
        snd_pcm_close(playback->pcm);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, snd_strerror(rc));
        return false;
    }

    rc = snd_pcm_hw_params_set_access(playback->pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (rc < 0) {
        snd_pcm_close(playback->pcm);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, snd_strerror(rc));
        return false;
    }

    snd_pcm_format_t formats[5];
    size_t num_formats = 0;
    if (playback->has_format) {
        if (playback->requested_format == ALSA_SAMPLE_FORMAT_S16_LE) {
            formats[0] = SND_PCM_FORMAT_S16_LE;
            num_formats = 1;
        } else if (playback->requested_format == ALSA_SAMPLE_FORMAT_S24_3_LE) {
            formats[0] = SND_PCM_FORMAT_S24_3LE;
            num_formats = 1;
        } else if (playback->requested_format == ALSA_SAMPLE_FORMAT_S24_4_LE) {
            formats[0] = SND_PCM_FORMAT_S24_LE;
            num_formats = 1;
        } else if (playback->requested_format == ALSA_SAMPLE_FORMAT_S32_LE) {
            formats[0] = SND_PCM_FORMAT_S32_LE;
            num_formats = 1;
        } else if (playback->requested_format == ALSA_SAMPLE_FORMAT_F32_LE) {
            formats[0] = SND_PCM_FORMAT_FLOAT_LE;
            num_formats = 1;
        } else if (playback->requested_format == ALSA_SAMPLE_FORMAT_F64_LE) {
            formats[0] = SND_PCM_FORMAT_FLOAT64_LE;
            num_formats = 1;
        }
    } else {
        formats[0] = SND_PCM_FORMAT_FLOAT_LE;
        formats[1] = SND_PCM_FORMAT_S32_LE;
        formats[2] = SND_PCM_FORMAT_S24_3LE;
        formats[3] = SND_PCM_FORMAT_S16_LE;
        num_formats = 4;
    }

    bool format_ok = false;
    for (size_t i = 0; i < num_formats; i++) {
        rc = snd_pcm_hw_params_set_format(playback->pcm, params, formats[i]);
        if (rc >= 0) {
            playback->format = formats[i];
            format_ok = true;
            break;
        }
    }
    if (!format_ok) {
        snd_pcm_close(playback->pcm);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Requested or supported ALSA format not available");
        return false;
    }

    rc = snd_pcm_hw_params_set_channels(playback->pcm, params, playback->channels);
    if (rc < 0) {
        snd_pcm_close(playback->pcm);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, snd_strerror(rc));
        return false;
    }

    unsigned int val = playback->sample_rate;
    int dir = 0;
    rc = snd_pcm_hw_params_set_rate_near(playback->pcm, params, &val, &dir);
    if (rc < 0) {
        snd_pcm_close(playback->pcm);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, snd_strerror(rc));
        return false;
    }

    snd_pcm_uframes_t period_size = playback->chunk_size;
    rc = snd_pcm_hw_params_set_period_size_near(playback->pcm, params, &period_size, &dir);
    if (rc < 0) {
        snd_pcm_close(playback->pcm);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, snd_strerror(rc));
        return false;
    }

    snd_pcm_uframes_t buffer_size = period_size * 4;
    rc = snd_pcm_hw_params_set_buffer_size_near(playback->pcm, params, &buffer_size);
    if (rc < 0) {
        snd_pcm_close(playback->pcm);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, snd_strerror(rc));
        return false;
    }

    rc = snd_pcm_hw_params(playback->pcm, params);
    if (rc < 0) {
        snd_pcm_close(playback->pcm);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, snd_strerror(rc));
        return false;
    }

    size_t sample_size = 4;
    if (playback->format == SND_PCM_FORMAT_S16_LE) {
        sample_size = 2;
    } else if (playback->format == SND_PCM_FORMAT_S24_3LE) {
        sample_size = 3;
    } else if (playback->format == SND_PCM_FORMAT_S24_LE) {
        sample_size = 4;
    } else if (playback->format == SND_PCM_FORMAT_FLOAT64_LE) {
        sample_size = 8;
    }
    playback->interleaved_buf_size = playback->chunk_size * playback->channels * sample_size;
    playback->interleaved_buf = malloc(playback->interleaved_buf_size);

    playback->paused = false;
    return true;
}

bool alsa_playback_write(alsa_playback_t* playback, const audio_chunk_t* chunk, backend_error_t* err) {
    if (!playback->pcm) return false;

    size_t frames = chunk->valid_frames;
    if (frames == 0) return true;

    if (playback->format == SND_PCM_FORMAT_FLOAT_LE) {
        float* dst = (float*)playback->interleaved_buf;
        for (size_t f = 0; f < frames; f++) {
            for (size_t c = 0; c < (size_t)playback->channels; c++) {
                double val = audio_chunk_get_channel(chunk, c)[f];
                dst[f * playback->channels + c] = (float)val;
            }
        }
    } else if (playback->format == SND_PCM_FORMAT_S32_LE) {
        int32_t* dst = (int32_t*)playback->interleaved_buf;
        for (size_t f = 0; f < frames; f++) {
            for (size_t c = 0; c < (size_t)playback->channels; c++) {
                double val = audio_chunk_get_channel(chunk, c)[f];
                if (val > 1.0) val = 1.0;
                else if (val < -1.0) val = -1.0;
                dst[f * playback->channels + c] = (int32_t)(val * 2147483647.0);
            }
        }
    } else if (playback->format == SND_PCM_FORMAT_S24_3LE) {
        uint8_t* dst = (uint8_t*)playback->interleaved_buf;
        for (size_t f = 0; f < frames; f++) {
            for (size_t c = 0; c < (size_t)playback->channels; c++) {
                double val = audio_chunk_get_channel(chunk, c)[f];
                if (val > 1.0) val = 1.0;
                else if (val < -1.0) val = -1.0;
                int32_t ival = (int32_t)(val * 8388607.0);
                size_t offset = (f * playback->channels + c) * 3;
                dst[offset] = ival & 0xFF;
                dst[offset+1] = (ival >> 8) & 0xFF;
                dst[offset+2] = (ival >> 16) & 0xFF;
            }
        }
    } else if (playback->format == SND_PCM_FORMAT_S24_LE) {
        int32_t* dst = (int32_t*)playback->interleaved_buf;
        for (size_t f = 0; f < frames; f++) {
            for (size_t c = 0; c < (size_t)playback->channels; c++) {
                double val = audio_chunk_get_channel(chunk, c)[f];
                if (val > 1.0) val = 1.0;
                else if (val < -1.0) val = -1.0;
                dst[f * playback->channels + c] = (int32_t)(val * 8388607.0);
            }
        }
    } else if (playback->format == SND_PCM_FORMAT_FLOAT64_LE) {
        double* dst = (double*)playback->interleaved_buf;
        for (size_t f = 0; f < frames; f++) {
            for (size_t c = 0; c < (size_t)playback->channels; c++) {
                dst[f * playback->channels + c] = audio_chunk_get_channel(chunk, c)[f];
            }
        }
    } else if (playback->format == SND_PCM_FORMAT_S16_LE) {
        int16_t* dst = (int16_t*)playback->interleaved_buf;
        for (size_t f = 0; f < frames; f++) {
            for (size_t c = 0; c < (size_t)playback->channels; c++) {
                double val = audio_chunk_get_channel(chunk, c)[f];
                if (val > 1.0) val = 1.0;
                else if (val < -1.0) val = -1.0;
                dst[f * playback->channels + c] = (int16_t)(val * 32767.0);
            }
        }
    }

    if (playback->paused) {
        return true;
    }

    snd_pcm_sframes_t rc = snd_pcm_writei(playback->pcm, playback->interleaved_buf, frames);
    if (rc < 0) {
        if (rc == -EPIPE) {
            snd_pcm_prepare(playback->pcm);
            rc = snd_pcm_writei(playback->pcm, playback->interleaved_buf, frames);
        }
        if (rc < 0) {
            if (err) backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, snd_strerror(rc));
            return false;
        }
    }

    return true;
}

void alsa_playback_close(alsa_playback_t* playback) {
    if (playback->pcm) {
        snd_pcm_drain(playback->pcm);
        snd_pcm_close(playback->pcm);
        playback->pcm = NULL;
    }
    if (playback->interleaved_buf) {
        free(playback->interleaved_buf);
        playback->interleaved_buf = NULL;
    }
}

size_t alsa_playback_get_buffer_level(alsa_playback_t* playback) {
    if (!playback->pcm) return 0;
    snd_pcm_sframes_t delay = 0;
    int err = snd_pcm_delay(playback->pcm, &delay);
    if (err < 0) {
        if (err == -EPIPE) {
            snd_pcm_prepare(playback->pcm);
        }
        return 0;
    }
    return delay < 0 ? 0 : (size_t)delay;
}

bool alsa_playback_get_pending_rate_change(alsa_playback_t* playback, double* out_rate) {
    (void)playback; (void)out_rate;
    return false;
}

bool alsa_playback_prefill_silence(alsa_playback_t* playback, size_t frames, backend_error_t* err) {
    if (!playback->pcm) return false;

    size_t sample_size = 4;
    if (playback->format == SND_PCM_FORMAT_S16_LE) {
        sample_size = 2;
    }

    size_t zero_buf_size = frames * playback->channels * sample_size;
    void* zero_buf = calloc(1, zero_buf_size);
    if (!zero_buf) return false;

    snd_pcm_sframes_t rc = snd_pcm_writei(playback->pcm, zero_buf, frames);
    free(zero_buf);

    if (rc < 0) {
        if (err) backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, snd_strerror(rc));
        return false;
    }
    return true;
}

bool alsa_playback_get_is_paused(alsa_playback_t* playback) {
    return playback->paused;
}

void alsa_playback_set_is_paused(alsa_playback_t* playback, bool paused) {
    if (!playback->pcm) return;
    playback->paused = paused;
    snd_pcm_pause(playback->pcm, paused ? 1 : 0);
}

void alsa_playback_destroy(alsa_playback_t* playback) {
    if (!playback) return;
    alsa_playback_close(playback);
    free(playback);
}

#endif // !__APPLE__
