#if defined(ENABLE_ALSA)
#include "alsa_playback.h"

#include <alloca.h>
#include <alsa/asoundlib.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "Logging/app_logger.h"
#include "alsa_device.h"

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
  _Atomic bool paused;
  bool currently_paused;

  void* interleaved_buf;
  size_t interleaved_buf_size;

  snd_mixer_t* mixer;
  snd_mixer_elem_t* pitch_elem;
  pthread_mutex_t mixer_mutex;
};

/**
 * @brief Open the ALSA playback device.
 *
 * Wrapper for alsa_playback_open to match the backend vtable.
 *
 * @param ctx Pointer to the alsa_playback_t context.
 * @param err Pointer to backend_error_t to receive error details.
 * @return true if successful, false otherwise.
 */
static bool vtable_open(void* ctx, backend_error_t* err) {
  return alsa_playback_open((alsa_playback_t*)ctx, err);
}

/**
 * @brief Write audio samples to the ALSA device.
 *
 * Wrapper for alsa_playback_write to match the backend vtable.
 *
 * @param ctx Pointer to the alsa_playback_t context.
 * @param chunk Audio chunk to write.
 * @param err Pointer to backend_error_t to receive error details.
 * @return true if successful, false otherwise.
 */
static bool vtable_write(void* ctx, const audio_chunk_t* chunk,
                         backend_error_t* err) {
  return alsa_playback_write((alsa_playback_t*)ctx, chunk, err);
}

/**
 * @brief Close the ALSA playback device.
 *
 * Wrapper for alsa_playback_close to match the backend vtable.
 *
 * @param ctx Pointer to the alsa_playback_t context.
 */
static void vtable_close(void* ctx) {
  alsa_playback_close((alsa_playback_t*)ctx);
}

/**
 * @brief Get the current ALSA buffer latency level.
 *
 * Wrapper for alsa_playback_get_buffer_level to match the backend vtable.
 *
 * @param ctx Pointer to the alsa_playback_t context.
 * @return Current buffer level in frames.
 */
static size_t vtable_get_buffer_level(void* ctx) {
  return alsa_playback_get_buffer_level((alsa_playback_t*)ctx);
}

/**
 * @brief Check for pending rate change.
 *
 * Wrapper for alsa_playback_get_pending_rate_change to match the backend
 * vtable.
 *
 * @param ctx Pointer to the alsa_playback_t context.
 * @param out_rate Pointer to receive the new rate.
 * @return true if a rate change is pending, false otherwise.
 */
static bool vtable_get_rate(void* ctx, double* out_rate) {
  return alsa_playback_get_pending_rate_change((alsa_playback_t*)ctx, out_rate);
}

/**
 * @brief Prefill ALSA buffer with silence.
 *
 * Wrapper for alsa_playback_prefill_silence to match the backend vtable.
 *
 * @param ctx Pointer to the alsa_playback_t context.
 * @param frames Number of silence frames to prefill.
 * @param err Pointer to backend_error_t to receive error details.
 * @return true if successful, false otherwise.
 */
static bool vtable_prefill(void* ctx, size_t frames, backend_error_t* err) {
  return alsa_playback_prefill_silence((alsa_playback_t*)ctx, frames, err);
}

/**
 * @brief Get the pause status of the ALSA playback.
 *
 * Wrapper for alsa_playback_get_is_paused to match the backend vtable.
 *
 * @param ctx Pointer to the alsa_playback_t context.
 * @return true if paused, false otherwise.
 */
static bool vtable_get_paused(void* ctx) {
  return alsa_playback_get_is_paused((alsa_playback_t*)ctx);
}

/**
 * @brief Set the pause status of the ALSA playback.
 *
 * Wrapper for alsa_playback_set_is_paused to match the backend vtable.
 *
 * @param ctx Pointer to the alsa_playback_t context.
 * @param paused true to pause, false to resume.
 */
static void vtable_set_paused(void* ctx, bool paused) {
  alsa_playback_set_is_paused((alsa_playback_t*)ctx, paused);
}

/**
 * @brief Destroy the ALSA playback context.
 *
 * Wrapper for alsa_playback_destroy to match the backend vtable.
 *
 * @param ctx Pointer to the alsa_playback_t context.
 */
static void vtable_destroy(void* ctx) {
  alsa_playback_destroy((alsa_playback_t*)ctx);
}

/**
 * @brief Check if pitch control is supported by the ALSA device.
 *
 * Wrapper for alsa_playback_pitch_control_supported to match the backend
 * vtable.
 *
 * @param ctx Pointer to the alsa_playback_t context.
 * @return true if pitch control is supported, false otherwise.
 */
static bool vtable_pitch_control_supported(void* ctx) {
  return alsa_playback_pitch_control_supported((alsa_playback_t*)ctx);
}

/**
 * @brief Set the pitch multiplier for playback.
 *
 * Wrapper for alsa_playback_set_pitch to match the backend vtable.
 *
 * @param ctx Pointer to the alsa_playback_t context.
 * @param mult Pitch multiplier.
 */
static void vtable_set_pitch(void* ctx, double mult) {
  alsa_playback_set_pitch((alsa_playback_t*)ctx, mult);
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
    .pitch_control_supported = vtable_pitch_control_supported,
    .set_pitch = vtable_set_pitch,
    .destroy = vtable_destroy};

playback_backend_t* alsa_playback_create(const playback_device_config_t* config,
                                         int sample_rate, int chunk_size,
                                         processing_parameters_t* params,
                                         backend_error_t* err) {
  (void)err;
  alsa_playback_t* playback =
      (alsa_playback_t*)calloc(1, sizeof(alsa_playback_t));
  if (!playback) return NULL;

  // Clean up name
  char clean_name[256];
  snprintf(clean_name, sizeof(clean_name), "%s",
           config->cfg.alsa.device[0] ? config->cfg.alsa.device : "default");
  char* space = strchr(clean_name, ' ');
  if (space) *space = '\0';
  snprintf(playback->device_name, sizeof(playback->device_name), "%s",
           clean_name);

  playback->sample_rate = sample_rate;
  playback->channels = config->cfg.alsa.channels;
  playback->chunk_size = chunk_size;

  playback->has_format = config->cfg.alsa.has_format;
  playback->requested_format = config->cfg.alsa.format;
  playback->params = params;
  atomic_init(&playback->paused, false);
  playback->currently_paused = false;
  pthread_mutex_init(&playback->mixer_mutex, NULL);

  playback_backend_t* backend =
      (playback_backend_t*)calloc(1, sizeof(playback_backend_t));
  if (!backend) {
    free(playback);
    return NULL;
  }
  backend->ctx = playback;
  backend->vtable = &ALSA_PLAYBACK_VTABLE;
  return backend;
}

bool alsa_playback_open(alsa_playback_t* playback, backend_error_t* err) {
  pthread_mutex_lock(&g_alsa_mutex);
  if (playback->pcm != NULL) {
    pthread_mutex_unlock(&g_alsa_mutex);
    return true;
  }
  int rc;
  rc = snd_pcm_open(&playback->pcm, playback->device_name,
                    SND_PCM_STREAM_PLAYBACK, 0);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    pthread_mutex_unlock(&g_alsa_mutex);
    return false;
  }

  snd_pcm_hw_params_t* params;
  snd_pcm_hw_params_alloca(&params);
  rc = snd_pcm_hw_params_any(playback->pcm, params);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  rc = snd_pcm_hw_params_set_access(playback->pcm, params,
                                    SND_PCM_ACCESS_RW_INTERLEAVED);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  // Select format: If a specific format was requested, attempt to use only
  // that. Otherwise, probe format support in order of preference: float32 ->
  // int32 -> int24 (3 bytes) -> int16.
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
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Requested or supported ALSA format not available");
    goto error_cleanup;
  }

  rc =
      snd_pcm_hw_params_set_channels(playback->pcm, params, playback->channels);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  unsigned int val = playback->sample_rate;
  int dir = 0;
  rc = snd_pcm_hw_params_set_rate_near(playback->pcm, params, &val, &dir);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  snd_pcm_uframes_t period_size = playback->chunk_size;
  rc = snd_pcm_hw_params_set_period_size_near(playback->pcm, params,
                                              &period_size, &dir);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  snd_pcm_uframes_t buffer_size = period_size * 4;
  rc = snd_pcm_hw_params_set_buffer_size_near(playback->pcm, params,
                                              &buffer_size);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  rc = snd_pcm_hw_params(playback->pcm, params);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  snd_pcm_sw_params_t* sw_params;
  snd_pcm_sw_params_alloca(&sw_params);
  rc = snd_pcm_sw_params_current(playback->pcm, sw_params);
  if (rc >= 0) {
    snd_pcm_sw_params_set_start_threshold(playback->pcm, sw_params, 1);
    snd_pcm_sw_params_set_avail_min(playback->pcm, sw_params,
                                    playback->chunk_size);
    rc = snd_pcm_sw_params(playback->pcm, sw_params);
    if (rc < 0) {
      logger_t logger = logger_create("dsp.backend.alsa");
      logger_warn(&logger, "Failed to set ALSA software parameters: %s",
                  log_arg_string(snd_strerror(rc)), log_arg_none(),
                  log_arg_none(), log_arg_none());
    }
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
  playback->interleaved_buf_size =
      playback->chunk_size * playback->channels * sample_size;
  playback->interleaved_buf = malloc(playback->interleaved_buf_size);
  if (!playback->interleaved_buf) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate ALSA playback interleaved buffer");
    goto error_cleanup;
  }

  playback->paused = false;

  // Initialize mixer for pitch control
  // Initialize mixer for pitch control.
  // Queries the ALSA hardware card related to the PCM device and attempts to
  // find a simple mixer control element named "Playback Pitch 1000000" which is
  // used to scale the playback speed.
  snd_pcm_info_t* pcm_info;
  snd_pcm_info_alloca(&pcm_info);
  if (snd_pcm_info(playback->pcm, pcm_info) >= 0) {
    char ctl_name[32];
    int card = snd_pcm_info_get_card(pcm_info);
    if (card >= 0) {
      snprintf(ctl_name, sizeof(ctl_name), "hw:%d", card);
      snd_mixer_t* mixer = NULL;
      if (snd_mixer_open(&mixer, 0) >= 0) {
        if (snd_mixer_attach(mixer, ctl_name) >= 0 &&
            snd_mixer_selem_register(mixer, NULL, NULL) >= 0 &&
            snd_mixer_load(mixer) >= 0) {
          pthread_mutex_lock(&playback->mixer_mutex);
          playback->mixer = mixer;

          snd_mixer_selem_id_t* sid;
          snd_mixer_selem_id_alloca(&sid);
          snd_mixer_selem_id_set_name(sid, "Playback Pitch 1000000");
          playback->pitch_elem = snd_mixer_find_selem(mixer, sid);
          pthread_mutex_unlock(&playback->mixer_mutex);
        } else {
          snd_mixer_close(mixer);
        }
      }
    }
  }

  pthread_mutex_unlock(&g_alsa_mutex);
  return true;

error_cleanup:
  if (playback->pcm) {
    snd_pcm_close(playback->pcm);
    playback->pcm = NULL;
  }
  if (playback->interleaved_buf) {
    free(playback->interleaved_buf);
    playback->interleaved_buf = NULL;
  }
  pthread_mutex_unlock(&g_alsa_mutex);
  return false;
}

bool alsa_playback_write(alsa_playback_t* playback, const audio_chunk_t* chunk,
                         backend_error_t* err) {
  if (!playback->pcm) return false;

  if (audio_chunk_get_channels(chunk) < (size_t)playback->channels) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Chunk channels count is smaller than playback device channels");
    }
    return false;
  }

  size_t frames = audio_chunk_get_valid_frames(chunk);
  if (frames == 0) return true;

  // Convert the input audio chunk (planar double format) to the target
  // interleaved format buffer. Clip samples to valid range when converting to
  // fixed-point formats.
  if (playback->format == SND_PCM_FORMAT_FLOAT_LE) {
    // Convert double to float.
    float* dst = (float*)playback->interleaved_buf;
    for (size_t f = 0; f < frames; f++) {
      for (size_t c = 0; c < (size_t)playback->channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[f];
        dst[f * playback->channels + c] = (float)val;
      }
    }
  } else if (playback->format == SND_PCM_FORMAT_S32_LE) {
    // Convert double to 32-bit signed integer.
    int32_t* dst = (int32_t*)playback->interleaved_buf;
    for (size_t f = 0; f < frames; f++) {
      for (size_t c = 0; c < (size_t)playback->channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[f];
        if (val > 1.0)
          val = 1.0;
        else if (val < -1.0)
          val = -1.0;
        dst[f * playback->channels + c] = (int32_t)(val * 2147483647.0);
      }
    }
  } else if (playback->format == SND_PCM_FORMAT_S24_3LE) {
    // Convert double to 24-bit signed integer, packed in 3 bytes.
    uint8_t* dst = (uint8_t*)playback->interleaved_buf;
    for (size_t f = 0; f < frames; f++) {
      for (size_t c = 0; c < (size_t)playback->channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[f];
        if (val > 1.0)
          val = 1.0;
        else if (val < -1.0)
          val = -1.0;
        int32_t ival = (int32_t)(val * 8388607.0);
        size_t offset = (f * playback->channels + c) * 3;
        dst[offset] = ival & 0xFF;
        dst[offset + 1] = (ival >> 8) & 0xFF;
        dst[offset + 2] = (ival >> 16) & 0xFF;
      }
    }
  } else if (playback->format == SND_PCM_FORMAT_S24_LE) {
    // Convert double to 24-bit signed integer inside 32-bit containers.
    int32_t* dst = (int32_t*)playback->interleaved_buf;
    for (size_t f = 0; f < frames; f++) {
      for (size_t c = 0; c < (size_t)playback->channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[f];
        if (val > 1.0)
          val = 1.0;
        else if (val < -1.0)
          val = -1.0;
        dst[f * playback->channels + c] = (int32_t)(val * 8388607.0);
      }
    }
  } else if (playback->format == SND_PCM_FORMAT_FLOAT64_LE) {
    // Direct copy for double format.
    double* dst = (double*)playback->interleaved_buf;
    for (size_t f = 0; f < frames; f++) {
      for (size_t c = 0; c < (size_t)playback->channels; c++) {
        dst[f * playback->channels + c] = audio_chunk_get_channel(chunk, c)[f];
      }
    }
  } else if (playback->format == SND_PCM_FORMAT_S16_LE) {
    // Convert double to 16-bit signed integer.
    int16_t* dst = (int16_t*)playback->interleaved_buf;
    for (size_t f = 0; f < frames; f++) {
      for (size_t c = 0; c < (size_t)playback->channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[f];
        if (val > 1.0)
          val = 1.0;
        else if (val < -1.0)
          val = -1.0;
        dst[f * playback->channels + c] = (int16_t)(val * 32767.0);
      }
    }
  }

  bool paused = atomic_load_explicit(&playback->paused, memory_order_acquire);
  if (paused) {
    if (!playback->currently_paused) {
      snd_pcm_pause(playback->pcm, 1);
      playback->currently_paused = true;
    }
    return true;
  } else {
    if (playback->currently_paused) {
      snd_pcm_pause(playback->pcm, 0);
      playback->currently_paused = false;
    }
  }

  // Write interleaved samples to ALSA device.
  // In case of write failure (e.g., underrun), attempt to recover and retry the
  // write once.
  snd_pcm_sframes_t rc =
      snd_pcm_writei(playback->pcm, playback->interleaved_buf, frames);
  if (rc < 0) {
    rc = snd_pcm_recover(playback->pcm, rc, 0);
    if (rc >= 0) {
      rc = snd_pcm_writei(playback->pcm, playback->interleaved_buf, frames);
    }
    if (rc < 0) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, snd_strerror(rc));
      return false;
    }
  }

  return true;
}

void alsa_playback_close(alsa_playback_t* playback) {
  if (!playback) return;
  if (playback->pcm) {
    snd_pcm_drain(playback->pcm);
    snd_pcm_close(playback->pcm);
    playback->pcm = NULL;
  }
  if (playback->interleaved_buf) {
    free(playback->interleaved_buf);
    playback->interleaved_buf = NULL;
  }
  pthread_mutex_lock(&playback->mixer_mutex);
  if (playback->mixer) {
    snd_mixer_close(playback->mixer);
    playback->mixer = NULL;
    playback->pitch_elem = NULL;
  }
  pthread_mutex_unlock(&playback->mixer_mutex);
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

bool alsa_playback_get_pending_rate_change(alsa_playback_t* playback,
                                           double* out_rate) {
  (void)playback;
  (void)out_rate;
  return false;
}

bool alsa_playback_prefill_silence(alsa_playback_t* playback, size_t frames,
                                   backend_error_t* err) {
  if (!playback->pcm) return false;

  int bits = snd_pcm_format_physical_width(playback->format);
  size_t sample_size = (bits > 0) ? ((size_t)bits / 8) : 4;

  size_t zero_buf_size = frames * playback->channels * sample_size;
  void* zero_buf = calloc(1, zero_buf_size);
  if (!zero_buf) return false;

  snd_pcm_sframes_t rc = snd_pcm_writei(playback->pcm, zero_buf, frames);
  free(zero_buf);

  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, snd_strerror(rc));
    return false;
  }
  return true;
}

bool alsa_playback_get_is_paused(alsa_playback_t* playback) {
  if (!playback) return false;
  return atomic_load_explicit(&playback->paused, memory_order_acquire);
}

void alsa_playback_set_is_paused(alsa_playback_t* playback, bool paused) {
  if (!playback) return;
  atomic_store_explicit(&playback->paused, paused, memory_order_release);
}

bool alsa_playback_pitch_control_supported(alsa_playback_t* playback) {
  if (!playback) return false;
  pthread_mutex_lock(&playback->mixer_mutex);
  bool res = playback->pitch_elem != NULL;
  pthread_mutex_unlock(&playback->mixer_mutex);
  return res;
}

void alsa_playback_set_pitch(alsa_playback_t* playback, double multiplier) {
  if (!playback) return;
  pthread_mutex_lock(&playback->mixer_mutex);
  if (!playback->pitch_elem) {
    pthread_mutex_unlock(&playback->mixer_mutex);
    return;
  }
  // Calculate raw pitch value. The pitch element expects a value mapped to
  // 1000000 / multiplier. A higher multiplier means higher pitch (faster
  // playback), which translates to a smaller interval value on the control.
  long value = (long)round(1000000.0 / multiplier);
  if (snd_mixer_selem_has_playback_volume(playback->pitch_elem)) {
    snd_mixer_selem_set_playback_volume_all(playback->pitch_elem, value);
  } else if (snd_mixer_selem_has_capture_volume(playback->pitch_elem)) {
    snd_mixer_selem_set_capture_volume_all(playback->pitch_elem, value);
  }
  pthread_mutex_unlock(&playback->mixer_mutex);
}

void alsa_playback_destroy(alsa_playback_t* playback) {
  if (!playback) return;
  alsa_playback_close(playback);
  pthread_mutex_destroy(&playback->mixer_mutex);
  free(playback);
}

#endif  // defined(ENABLE_ALSA)
