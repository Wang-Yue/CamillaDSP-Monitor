#ifndef __APPLE__
#define _GNU_SOURCE

#include <time.h>
#include <alloca.h>
#include <string.h>
#include "alsa_capture.h"
#include <alsa/asoundlib.h>
#include <stdlib.h>
#include <string.h>

struct alsa_capture {
    char device_name[256];
    int sample_rate;
    int channels;
    int chunk_size;

    snd_pcm_t* pcm;
    snd_pcm_format_t format;

    void* interleaved_buf;
    size_t interleaved_buf_size;
};

static bool vtable_open(void* ctx, backend_error_t* err) {
    return alsa_capture_open((alsa_capture_t*)ctx, err);
}

static bool vtable_read(void* ctx, size_t frames, audio_chunk_t* chunk, backend_error_t* err) {
    return alsa_capture_read((alsa_capture_t*)ctx, frames, chunk, err);
}

static void vtable_close(void* ctx) {
    alsa_capture_close((alsa_capture_t*)ctx);
}

static bool vtable_get_rate(void* ctx, double* out_rate) {
    return alsa_capture_get_pending_rate_change((alsa_capture_t*)ctx, out_rate);
}

static bool vtable_pitch_supp(void* ctx) {
    return alsa_capture_pitch_control_supported((alsa_capture_t*)ctx);
}

static void vtable_set_pitch(void* ctx, double mult) {
    alsa_capture_set_pitch((alsa_capture_t*)ctx, mult);
}

static bool vtable_wait(void* ctx, uint32_t t) {
    return alsa_capture_wait((alsa_capture_t*)ctx, t);
}

static void vtable_destroy(void* ctx) {
    alsa_capture_destroy((alsa_capture_t*)ctx);
}

static const capture_backend_vtable_t ALSA_CAPTURE_VTABLE = {
    .open = vtable_open,
    .read = vtable_read,
    .close = vtable_close,
    .get_pending_rate_change = vtable_get_rate,
    .is_pitch_control_supported = vtable_pitch_supp,
    .set_pitch = vtable_set_pitch,
    .wait_for_data = vtable_wait,
    .destroy = vtable_destroy
};

capture_backend_t* alsa_capture_create(const capture_device_config_t* config, int sample_rate, int chunk_size, backend_error_t* err) {
    (void)err;
    alsa_capture_t* capture = (alsa_capture_t*)calloc(1, sizeof(alsa_capture_t));
    if (!capture) return NULL;

    // Clean up name
    char clean_name[256];
    snprintf(clean_name, sizeof(clean_name), "%s", config->device[0] ? config->device : "default");
    char* space = strchr(clean_name, ' ');
    if (space) *space = '\0';
    snprintf(capture->device_name, sizeof(capture->device_name), "%s", clean_name);

    capture->sample_rate = sample_rate;
    capture->channels = config->channels;
    capture->chunk_size = chunk_size;

    capture_backend_t* backend = (capture_backend_t*)calloc(1, sizeof(capture_backend_t));
    if (!backend) {
        free(capture);
        return NULL;
    }
    backend->ctx = capture;
    backend->vtable = &ALSA_CAPTURE_VTABLE;
    return backend;
}

bool alsa_capture_open(alsa_capture_t* capture, backend_error_t* err) {
    int rc;
    rc = snd_pcm_open(&capture->pcm, capture->device_name, SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, snd_strerror(rc));
        return false;
    }

    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);
    rc = snd_pcm_hw_params_any(capture->pcm, params);
    if (rc < 0) {
        snd_pcm_close(capture->pcm);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, snd_strerror(rc));
        return false;
    }

    rc = snd_pcm_hw_params_set_access(capture->pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (rc < 0) {
        snd_pcm_close(capture->pcm);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, snd_strerror(rc));
        return false;
    }

    snd_pcm_format_t formats[] = {
        SND_PCM_FORMAT_FLOAT_LE,
        SND_PCM_FORMAT_S32_LE,
        SND_PCM_FORMAT_S16_LE
    };
    bool format_ok = false;
    for (size_t i = 0; i < sizeof(formats)/sizeof(formats[0]); i++) {
        rc = snd_pcm_hw_params_set_format(capture->pcm, params, formats[i]);
        if (rc >= 0) {
            capture->format = formats[i];
            format_ok = true;
            break;
        }
    }
    if (!format_ok) {
        snd_pcm_close(capture->pcm);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "No supported ALSA format (Float, S32, S16) available");
        return false;
    }

    rc = snd_pcm_hw_params_set_channels(capture->pcm, params, capture->channels);
    if (rc < 0) {
        snd_pcm_close(capture->pcm);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, snd_strerror(rc));
        return false;
    }

    unsigned int val = capture->sample_rate;
    int dir = 0;
    rc = snd_pcm_hw_params_set_rate_near(capture->pcm, params, &val, &dir);
    if (rc < 0) {
        snd_pcm_close(capture->pcm);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, snd_strerror(rc));
        return false;
    }

    snd_pcm_uframes_t period_size = capture->chunk_size;
    rc = snd_pcm_hw_params_set_period_size_near(capture->pcm, params, &period_size, &dir);
    if (rc < 0) {
        snd_pcm_close(capture->pcm);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, snd_strerror(rc));
        return false;
    }

    snd_pcm_uframes_t buffer_size = period_size * 4;
    rc = snd_pcm_hw_params_set_buffer_size_near(capture->pcm, params, &buffer_size);
    if (rc < 0) {
        snd_pcm_close(capture->pcm);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, snd_strerror(rc));
        return false;
    }

    rc = snd_pcm_hw_params(capture->pcm, params);
    if (rc < 0) {
        snd_pcm_close(capture->pcm);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, snd_strerror(rc));
        return false;
    }

    size_t sample_size = 4;
    if (capture->format == SND_PCM_FORMAT_S16_LE) {
        sample_size = 2;
    }
    capture->interleaved_buf_size = capture->chunk_size * capture->channels * sample_size;
    capture->interleaved_buf = malloc(capture->interleaved_buf_size);

    return true;
}

bool alsa_capture_read(alsa_capture_t* capture, size_t frames, audio_chunk_t* chunk, backend_error_t* err) {
    if (!capture->pcm) return false;
    if (frames > (size_t)capture->chunk_size) {
        frames = capture->chunk_size;
    }

    snd_pcm_sframes_t rc = snd_pcm_readi(capture->pcm, capture->interleaved_buf, frames);
    if (rc < 0) {
        if (rc == -EPIPE) {
            snd_pcm_prepare(capture->pcm);
            rc = snd_pcm_readi(capture->pcm, capture->interleaved_buf, frames);
        }
        if (rc < 0) {
            if (err) backend_error_init(err, BACKEND_ERROR_READ_ERROR, snd_strerror(rc));
            return false;
        }
    }

    size_t read_frames = rc;
    chunk->valid_frames = read_frames;

    if (capture->format == SND_PCM_FORMAT_FLOAT_LE) {
        float* src = (float*)capture->interleaved_buf;
        for (size_t f = 0; f < read_frames; f++) {
            for (size_t c = 0; c < (size_t)capture->channels; c++) {
                double* dst = audio_chunk_get_channel(chunk, c);
                dst[f] = src[f * capture->channels + c];
            }
        }
    } else if (capture->format == SND_PCM_FORMAT_S32_LE) {
        int32_t* src = (int32_t*)capture->interleaved_buf;
        double scale = 1.0 / 2147483648.0;
        for (size_t f = 0; f < read_frames; f++) {
            for (size_t c = 0; c < (size_t)capture->channels; c++) {
                double* dst = audio_chunk_get_channel(chunk, c);
                dst[f] = (double)src[f * capture->channels + c] * scale;
            }
        }
    } else if (capture->format == SND_PCM_FORMAT_S16_LE) {
        int16_t* src = (int16_t*)capture->interleaved_buf;
        double scale = 1.0 / 32768.0;
        for (size_t f = 0; f < read_frames; f++) {
            for (size_t c = 0; c < (size_t)capture->channels; c++) {
                double* dst = audio_chunk_get_channel(chunk, c);
                dst[f] = (double)src[f * capture->channels + c] * scale;
            }
        }
    }

    return true;
}

void alsa_capture_close(alsa_capture_t* capture) {
    if (capture->pcm) {
        snd_pcm_close(capture->pcm);
        capture->pcm = NULL;
    }
    if (capture->interleaved_buf) {
        free(capture->interleaved_buf);
        capture->interleaved_buf = NULL;
    }
}

bool alsa_capture_get_pending_rate_change(alsa_capture_t* capture, double* out_rate) {
    (void)capture; (void)out_rate;
    return false;
}

bool alsa_capture_pitch_control_supported(alsa_capture_t* capture) {
    (void)capture;
    return false;
}

void alsa_capture_set_pitch(alsa_capture_t* capture, double multiplier) {
    (void)capture; (void)multiplier;
}

bool alsa_capture_wait(alsa_capture_t* capture, uint32_t timeout_ms) {
    if (!capture->pcm) return false;
    int err = snd_pcm_wait(capture->pcm, (int)timeout_ms);
    return err > 0;
}

void alsa_capture_destroy(alsa_capture_t* capture) {
    if (!capture) return;
    alsa_capture_close(capture);
    free(capture);
}

#endif // !__APPLE__
