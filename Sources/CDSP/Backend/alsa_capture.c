#if defined(ENABLE_ALSA)
#include "alsa_capture.h"

#include <alloca.h>
#include <alsa/asoundlib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "Audio/processing_parameters.h"
#include "Logging/app_logger.h"
#include "alsa_device.h"

struct alsa_capture {
  char device_name[256];
  int sample_rate;
  int channels;
  int chunk_size;

  bool has_format;
  alsa_sample_format_t requested_format;
  bool stop_on_inactive;
  char link_volume_control[256];
  char link_mute_control[256];

  processing_parameters_t* params;
  snd_ctl_t* ctl;
  snd_mixer_t* mixer;
  snd_mixer_elem_t* vol_elem;
  snd_mixer_elem_t* mute_elem;
  snd_mixer_elem_t* pitch_elem;
  double last_synced_volume;
  bool last_synced_mute;

  snd_pcm_t* pcm;
  snd_pcm_format_t format;

  void* interleaved_buf;
  size_t interleaved_buf_size;
  pthread_mutex_t mixer_mutex;
};

/**
 * @brief Wrapper for opening the ALSA capture backend.
 */
static bool vtable_open(void* ctx, backend_error_t* err) {
  return alsa_capture_open((alsa_capture_t*)ctx, err);
}

/**
 * @brief Wrapper for reading audio frames from ALSA.
 */
static bool vtable_read(void* ctx, size_t frames, audio_chunk_t* chunk,
                        backend_error_t* err) {
  return alsa_capture_read((alsa_capture_t*)ctx, frames, chunk, err);
}

/**
 * @brief Wrapper for closing the ALSA capture backend.
 */
static void vtable_close(void* ctx) {
  alsa_capture_close((alsa_capture_t*)ctx);
}

/**
 * @brief Wrapper for querying pending rate changes.
 */
static bool vtable_get_rate(void* ctx, double* out_rate) {
  return alsa_capture_get_pending_rate_change((alsa_capture_t*)ctx, out_rate);
}

/**
 * @brief Wrapper for checking if pitch control is supported.
 */
static bool vtable_pitch_supp(void* ctx) {
  return alsa_capture_pitch_control_supported((alsa_capture_t*)ctx);
}

/**
 * @brief Wrapper for setting pitch multiplier.
 */
static void vtable_set_pitch(void* ctx, double mult) {
  alsa_capture_set_pitch((alsa_capture_t*)ctx, mult);
}

/**
 * @brief Wrapper for waiting for capture data to be available.
 */
static bool vtable_wait(void* ctx, uint32_t t) {
  return alsa_capture_wait((alsa_capture_t*)ctx, t);
}

/**
 * @brief Wrapper for destroying the ALSA capture backend instance.
 */
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
    .destroy = vtable_destroy};

capture_backend_t* alsa_capture_create(const capture_device_config_t* config,
                                       int sample_rate, int chunk_size,
                                       processing_parameters_t* params,
                                       backend_error_t* err) {
  (void)err;
  alsa_capture_t* capture = (alsa_capture_t*)calloc(1, sizeof(alsa_capture_t));
  if (!capture) return NULL;

  // Clean up name
  char clean_name[256];
  snprintf(clean_name, sizeof(clean_name), "%s",
           config->cfg.alsa.device[0] ? config->cfg.alsa.device : "default");
  char* space = strchr(clean_name, ' ');
  if (space) *space = '\0';
  snprintf(capture->device_name, sizeof(capture->device_name), "%s",
           clean_name);

  capture->sample_rate = sample_rate;
  capture->channels = config->cfg.alsa.channels;
  capture->chunk_size = chunk_size;

  capture->has_format = config->cfg.alsa.has_format;
  capture->requested_format = config->cfg.alsa.format;
  capture->params = params;
  capture->stop_on_inactive = config->cfg.alsa.stop_on_inactive;
  snprintf(capture->link_volume_control, sizeof(capture->link_volume_control),
           "%s", config->cfg.alsa.link_volume_control);
  snprintf(capture->link_mute_control, sizeof(capture->link_mute_control), "%s",
           config->cfg.alsa.link_mute_control);
  pthread_mutex_init(&capture->mixer_mutex, NULL);

  capture_backend_t* backend =
      (capture_backend_t*)calloc(1, sizeof(capture_backend_t));
  if (!backend) {
    free(capture);
    return NULL;
  }
  backend->ctx = capture;
  backend->vtable = &ALSA_CAPTURE_VTABLE;
  return backend;
}

/**
 * @brief Helper to query the volume of a mixer element in decibels.
 *
 * Retrieves the current playback or capture volume of the given mixer element,
 * and converts the raw integer volume range to decibels.
 *
 * @param elem The ALSA mixer element.
 * @return Volume in decibels, or -100.0 if muted/extremely quiet, or 0.0 if not
 * supported.
 */
static double get_elem_volume_db(snd_mixer_elem_t* elem) {
  if (!elem) return 0.0;
  long val = 0;
  long min = 0, max = 0;
  long db_val = 0;
  if (snd_mixer_selem_has_playback_volume(elem)) {
    if (snd_mixer_selem_get_playback_dB(elem, SND_MIXER_SCHN_FRONT_LEFT,
                                        &db_val) >= 0) {
      return (double)db_val / 100.0;
    }
    snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
    snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &val);
  } else if (snd_mixer_selem_has_capture_volume(elem)) {
    if (snd_mixer_selem_get_capture_dB(elem, SND_MIXER_SCHN_FRONT_LEFT,
                                       &db_val) >= 0) {
      return (double)db_val / 100.0;
    }
    snd_mixer_selem_get_capture_volume_range(elem, &min, &max);
    snd_mixer_selem_get_capture_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &val);
  } else {
    return 0.0;
  }
  if (max == min) return 0.0;
  double ratio = (double)(val - min) / (double)(max - min);
  if (ratio <= 0.0001) return -100.0;
  return 20.0 * log10(ratio);
}

/**
 * @brief Helper to set the volume of a mixer element in decibels.
 *
 * Converts the decibel value and updates the playback or capture volume on
 * all channels of the given mixer element.
 *
 * @param elem The ALSA mixer element.
 * @param db_val Volume in decibels.
 */
static void set_elem_volume_db(snd_mixer_elem_t* elem, double db_val) {
  if (!elem) return;
  long raw_db = (long)(db_val * 100.0);
  if (snd_mixer_selem_has_playback_volume(elem)) {
    snd_mixer_selem_set_playback_dB_all(elem, raw_db, 0);
  } else if (snd_mixer_selem_has_capture_volume(elem)) {
    snd_mixer_selem_set_capture_dB_all(elem, raw_db, 0);
  }
}

/**
 * @brief Helper to query the mute status of a mixer element.
 *
 * Checks if the switch for playback or capture is off (muted).
 *
 * @param elem The ALSA mixer element.
 * @return True if muted, false otherwise.
 */
static bool get_elem_mute(snd_mixer_elem_t* elem) {
  if (!elem) return false;
  int val = 1;
  if (snd_mixer_selem_has_playback_switch(elem)) {
    snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &val);
  } else if (snd_mixer_selem_has_capture_switch(elem)) {
    snd_mixer_selem_get_capture_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &val);
  }
  return val == 0;
}

/**
 * @brief Helper to set the mute status of a mixer element.
 *
 * Sets the switch for playback or capture on all channels.
 *
 * @param elem The ALSA mixer element.
 * @param mute True to mute (turn switch off), false to unmute.
 */
static void set_elem_mute(snd_mixer_elem_t* elem, bool mute) {
  if (!elem) return;
  int val = mute ? 0 : 1;
  if (snd_mixer_selem_has_playback_switch(elem)) {
    snd_mixer_selem_set_playback_switch_all(elem, val);
  } else if (snd_mixer_selem_has_capture_switch(elem)) {
    snd_mixer_selem_set_capture_switch_all(elem, val);
  }
}

/**
 * @brief Initialize ALSA control and mixer interfaces for volume/mute linking.
 *
 * Subscribes to control events to listen for hardware changes, attaches
 * the mixer, and looks up the specified volume, mute, and pitch control
 * elements. Initial values are synchronized to the engine.
 *
 * @param capture Pointer to the ALSA capture backend instance.
 */
static void alsa_capture_init_controls(alsa_capture_t* capture) {
  if (!capture->pcm) return;

  snd_pcm_info_t* info;
  snd_pcm_info_alloca(&info);
  if (snd_pcm_info(capture->pcm, info) < 0) return;

  int card = snd_pcm_info_get_card(info);
  if (card < 0) return;

  char ctl_name[32];
  snprintf(ctl_name, sizeof(ctl_name), "hw:%d", card);

  pthread_mutex_lock(&capture->mixer_mutex);

  // Open control interface (non-blocking)
  snd_ctl_t* ctl = NULL;
  if (snd_ctl_open(&ctl, ctl_name, SND_CTL_NONBLOCK) >= 0) {
    capture->ctl = ctl;
    snd_ctl_subscribe_events(ctl, 1);
  }

  // Open simple mixer interface
  snd_mixer_t* mixer = NULL;
  if (snd_mixer_open(&mixer, 0) >= 0) {
    if (snd_mixer_attach(mixer, ctl_name) >= 0 &&
        snd_mixer_selem_register(mixer, NULL, NULL) >= 0 &&
        snd_mixer_load(mixer) >= 0) {
      capture->mixer = mixer;

      // Find volume element
      if (capture->link_volume_control[0]) {
        snd_mixer_selem_id_t* sid;
        snd_mixer_selem_id_alloca(&sid);
        snd_mixer_selem_id_set_name(sid, capture->link_volume_control);
        capture->vol_elem = snd_mixer_find_selem(mixer, sid);
        if (capture->vol_elem) {
          capture->last_synced_volume = get_elem_volume_db(capture->vol_elem);
          processing_parameters_set_target_volume(capture->params,
                                                  capture->last_synced_volume);
        }
      }

      // Find mute element
      if (capture->link_mute_control[0]) {
        snd_mixer_selem_id_t* sid;
        snd_mixer_selem_id_alloca(&sid);
        snd_mixer_selem_id_set_name(sid, capture->link_mute_control);
        capture->mute_elem = snd_mixer_find_selem(mixer, sid);
        if (capture->mute_elem) {
          capture->last_synced_mute = get_elem_mute(capture->mute_elem);
          processing_parameters_set_muted(capture->params,
                                          capture->last_synced_mute);
        }
      }
      // Find pitch element
      {
        snd_mixer_selem_id_t* sid;
        snd_mixer_selem_id_alloca(&sid);
        snd_mixer_selem_id_set_name(sid, "Capture Pitch 1000000");
        capture->pitch_elem = snd_mixer_find_selem(mixer, sid);
      }
    } else {
      snd_mixer_close(mixer);
    }
  }
  pthread_mutex_unlock(&capture->mixer_mutex);
}

/**
 * @brief Synchronize mixer settings between the engine and ALSA hardware.
 *
 * Processes pending ALSA control events. Checks if the hardware mixer settings
 * (volume/mute) changed and updates the engine. Conversely, if engine target
 * faders changed, updates the hardware mixer values.
 *
 * @param capture Pointer to the ALSA capture backend instance.
 */
static void alsa_capture_sync_controls(alsa_capture_t* capture) {
  pthread_mutex_lock(&capture->mixer_mutex);
  if (!capture->mixer) {
    pthread_mutex_unlock(&capture->mixer_mutex);
    return;
  }

  if (capture->ctl) {
    snd_ctl_event_t* event;
    snd_ctl_event_alloca(&event);
    while (snd_ctl_read(capture->ctl, event) > 0) {
      if (snd_ctl_event_get_type(event) == SND_CTL_EVENT_ELEM) {
        unsigned int mask = snd_ctl_event_elem_get_mask(event);
        if (mask & SND_CTL_EVENT_MASK_VALUE) {
          snd_mixer_handle_events(capture->mixer);
        }
      }
    }
  }

  // Sync hardware to engine faders
  if (capture->vol_elem) {
    double hw_vol = get_elem_volume_db(capture->vol_elem);
    if (hw_vol != capture->last_synced_volume) {
      processing_parameters_set_target_volume(capture->params, hw_vol);
      capture->last_synced_volume = hw_vol;
    }
  }
  if (capture->mute_elem) {
    bool hw_mute = get_elem_mute(capture->mute_elem);
    if (hw_mute != capture->last_synced_mute) {
      processing_parameters_set_muted(capture->params, hw_mute);
      capture->last_synced_mute = hw_mute;
    }
  }

  // Sync engine faders to hardware
  double engine_vol = processing_parameters_get_target_volume(capture->params);
  if (engine_vol != capture->last_synced_volume) {
    set_elem_volume_db(capture->vol_elem, engine_vol);
    capture->last_synced_volume = engine_vol;
  }
  bool engine_mute = processing_parameters_is_muted(capture->params);
  if (engine_mute != capture->last_synced_mute) {
    set_elem_mute(capture->mute_elem, engine_mute);
    capture->last_synced_mute = engine_mute;
  }
  pthread_mutex_unlock(&capture->mixer_mutex);
}

bool alsa_capture_open(alsa_capture_t* capture, backend_error_t* err) {
  pthread_mutex_lock(&g_alsa_mutex);
  if (capture->pcm != NULL) {
    pthread_mutex_unlock(&g_alsa_mutex);
    return true;
  }
  int rc;
  // Open the ALSA PCM capture device
  rc = snd_pcm_open(&capture->pcm, capture->device_name, SND_PCM_STREAM_CAPTURE,
                    0);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    pthread_mutex_unlock(&g_alsa_mutex);
    return false;
  }

  snd_pcm_hw_params_t* params;
  snd_pcm_hw_params_alloca(&params);
  rc = snd_pcm_hw_params_any(capture->pcm, params);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  // Set access type to interleaved read/write
  rc = snd_pcm_hw_params_set_access(capture->pcm, params,
                                    SND_PCM_ACCESS_RW_INTERLEAVED);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  // Probe supported formats, trying requested format first, then falling back
  // to defaults. We prefer float, then S32, S24_3, S16.
  snd_pcm_format_t formats[5];
  size_t num_formats = 0;
  if (capture->has_format) {
    if (capture->requested_format == ALSA_SAMPLE_FORMAT_S16_LE) {
      formats[0] = SND_PCM_FORMAT_S16_LE;
      num_formats = 1;
    } else if (capture->requested_format == ALSA_SAMPLE_FORMAT_S24_3_LE) {
      formats[0] = SND_PCM_FORMAT_S24_3LE;
      num_formats = 1;
    } else if (capture->requested_format == ALSA_SAMPLE_FORMAT_S24_4_LE) {
      formats[0] = SND_PCM_FORMAT_S24_LE;
      num_formats = 1;
    } else if (capture->requested_format == ALSA_SAMPLE_FORMAT_S32_LE) {
      formats[0] = SND_PCM_FORMAT_S32_LE;
      num_formats = 1;
    } else if (capture->requested_format == ALSA_SAMPLE_FORMAT_F32_LE) {
      formats[0] = SND_PCM_FORMAT_FLOAT_LE;
      num_formats = 1;
    } else if (capture->requested_format == ALSA_SAMPLE_FORMAT_F64_LE) {
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
    rc = snd_pcm_hw_params_set_format(capture->pcm, params, formats[i]);
    if (rc >= 0) {
      capture->format = formats[i];
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

  // Set channel count
  rc = snd_pcm_hw_params_set_channels(capture->pcm, params, capture->channels);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  // Set sample rate (accepting nearest rate supported by hardware)
  unsigned int val = capture->sample_rate;
  int dir = 0;
  rc = snd_pcm_hw_params_set_rate_near(capture->pcm, params, &val, &dir);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  // Set period size (chunk size) near target
  snd_pcm_uframes_t period_size = capture->chunk_size;
  rc = snd_pcm_hw_params_set_period_size_near(capture->pcm, params,
                                              &period_size, &dir);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  // Set buffer size to 4x period size to avoid xruns
  snd_pcm_uframes_t buffer_size = period_size * 4;
  rc = snd_pcm_hw_params_set_buffer_size_near(capture->pcm, params,
                                              &buffer_size);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  rc = snd_pcm_hw_params(capture->pcm, params);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  // Set ALSA software parameters (e.g. start threshold, minimum available
  // frames)
  snd_pcm_sw_params_t* sw_params;
  snd_pcm_sw_params_alloca(&sw_params);
  rc = snd_pcm_sw_params_current(capture->pcm, sw_params);
  if (rc >= 0) {
    snd_pcm_sw_params_set_start_threshold(capture->pcm, sw_params, 0);
    snd_pcm_sw_params_set_avail_min(capture->pcm, sw_params,
                                    capture->chunk_size);
    rc = snd_pcm_sw_params(capture->pcm, sw_params);
    if (rc < 0) {
      logger_t logger = logger_create("dsp.backend.alsa");
      logger_warn(&logger, "Failed to set ALSA software parameters: %s",
                  log_arg_string(snd_strerror(rc)), log_arg_none(),
                  log_arg_none(), log_arg_none());
    }
  }

  // Determine sample size in bytes for the interleaved buffer allocation
  size_t sample_size = 4;
  if (capture->format == SND_PCM_FORMAT_S16_LE) {
    sample_size = 2;
  } else if (capture->format == SND_PCM_FORMAT_S24_3LE) {
    sample_size = 3;
  } else if (capture->format == SND_PCM_FORMAT_S24_LE) {
    sample_size = 4;
  } else if (capture->format == SND_PCM_FORMAT_FLOAT64_LE) {
    sample_size = 8;
  }
  capture->interleaved_buf_size =
      capture->chunk_size * capture->channels * sample_size;
  capture->interleaved_buf = malloc(capture->interleaved_buf_size);
  if (!capture->interleaved_buf) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate ALSA capture interleaved buffer");
    goto error_cleanup;
  }

  alsa_capture_init_controls(capture);

  pthread_mutex_unlock(&g_alsa_mutex);
  return true;

error_cleanup:
  if (capture->pcm) {
    snd_pcm_close(capture->pcm);
    capture->pcm = NULL;
  }
  pthread_mutex_unlock(&g_alsa_mutex);
  return false;
}

bool alsa_capture_read(alsa_capture_t* capture, size_t frames,
                       audio_chunk_t* chunk, backend_error_t* err) {
  if (!capture->pcm) return false;

  if (audio_chunk_get_channels(chunk) < (size_t)capture->channels) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Chunk channels count is smaller than capture device channels");
    }
    return false;
  }

  // Sync volume/mute between hardware and engine before reading
  alsa_capture_sync_controls(capture);

  if (frames > (size_t)capture->chunk_size) {
    frames = capture->chunk_size;
  }

  // Read interleaved frames from ALSA
  snd_pcm_sframes_t rc =
      snd_pcm_readi(capture->pcm, capture->interleaved_buf, frames);
  if (rc < 0) {
    // Attempt recovery on error (e.g., EPIPE for overrun) and retry read
    rc = snd_pcm_recover(capture->pcm, rc, 0);
    if (rc >= 0) {
      rc = snd_pcm_readi(capture->pcm, capture->interleaved_buf, frames);
    }
    if (rc < 0) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_READ_ERROR, snd_strerror(rc));
      return false;
    }
  }

  size_t read_frames = rc;
  audio_chunk_set_valid_frames(chunk, read_frames);

  // Convert and de-interleave samples to the output audio chunk.
  // Normalizes integer types to double [-1.0, 1.0].
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
  } else if (capture->format == SND_PCM_FORMAT_S24_3LE) {
    // Handle 24-bit 3-byte format (requires sign extension)
    uint8_t* src = (uint8_t*)capture->interleaved_buf;
    double scale = 1.0 / 8388608.0;
    for (size_t f = 0; f < read_frames; f++) {
      for (size_t c = 0; c < (size_t)capture->channels; c++) {
        size_t offset = (f * capture->channels + c) * 3;
        // Pack 3 bytes into a 32-bit int
        int32_t val =
            (src[offset] | (src[offset + 1] << 8) | (src[offset + 2] << 16));
        // Sign extend from 24-bit to 32-bit
        if (val & 0x800000) {
          val |= 0xFF000000;
        }
        double* dst = audio_chunk_get_channel(chunk, c);
        dst[f] = (double)val * scale;
      }
    }
  } else if (capture->format == SND_PCM_FORMAT_S24_LE) {
    int32_t* src = (int32_t*)capture->interleaved_buf;
    double scale = 1.0 / 8388608.0;
    for (size_t f = 0; f < read_frames; f++) {
      for (size_t c = 0; c < (size_t)capture->channels; c++) {
        double* dst = audio_chunk_get_channel(chunk, c);
        dst[f] = (double)src[f * capture->channels + c] * scale;
      }
    }
  } else if (capture->format == SND_PCM_FORMAT_FLOAT64_LE) {
    double* src = (double*)capture->interleaved_buf;
    for (size_t f = 0; f < read_frames; f++) {
      for (size_t c = 0; c < (size_t)capture->channels; c++) {
        double* dst = audio_chunk_get_channel(chunk, c);
        dst[f] = src[f * capture->channels + c];
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
  if (!capture) return;
  if (capture->pcm) {
    snd_pcm_close(capture->pcm);
    capture->pcm = NULL;
  }
  pthread_mutex_lock(&capture->mixer_mutex);
  if (capture->ctl) {
    snd_ctl_close(capture->ctl);
    capture->ctl = NULL;
  }
  if (capture->mixer) {
    snd_mixer_close(capture->mixer);
    capture->mixer = NULL;
  }
  capture->vol_elem = NULL;
  capture->mute_elem = NULL;
  capture->pitch_elem = NULL;
  pthread_mutex_unlock(&capture->mixer_mutex);
  if (capture->interleaved_buf) {
    free(capture->interleaved_buf);
    capture->interleaved_buf = NULL;
  }
}

bool alsa_capture_get_pending_rate_change(alsa_capture_t* capture,
                                          double* out_rate) {
  (void)capture;
  (void)out_rate;
  return false;
}

bool alsa_capture_pitch_control_supported(alsa_capture_t* capture) {
  return capture->pitch_elem != NULL;
}

void alsa_capture_set_pitch(alsa_capture_t* capture, double multiplier) {
  pthread_mutex_lock(&capture->mixer_mutex);
  if (!capture->pitch_elem) {
    pthread_mutex_unlock(&capture->mixer_mutex);
    return;
  }
  long value = (long)round(multiplier * 1000000.0);
  if (snd_mixer_selem_has_playback_volume(capture->pitch_elem)) {
    snd_mixer_selem_set_playback_volume_all(capture->pitch_elem, value);
  } else if (snd_mixer_selem_has_capture_volume(capture->pitch_elem)) {
    snd_mixer_selem_set_capture_volume_all(capture->pitch_elem, value);
  }
  pthread_mutex_unlock(&capture->mixer_mutex);
}

bool alsa_capture_wait(alsa_capture_t* capture, uint32_t timeout_ms) {
  if (!capture->pcm) return false;
  int err = snd_pcm_wait(capture->pcm, (int)timeout_ms);
  return err > 0;
}

void alsa_capture_destroy(alsa_capture_t* capture) {
  if (!capture) return;
  alsa_capture_close(capture);
  pthread_mutex_destroy(&capture->mixer_mutex);
  free(capture);
}

#endif  // defined(ENABLE_ALSA)
