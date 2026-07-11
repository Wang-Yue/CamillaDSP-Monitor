#include "dsp_engine.h"

#include <pthread.h>

#include "Audio/audio_history_buffer.h"
#include "dsp_engine_core.h"
#include "Config/config_diff.h"

struct dsp_engine {
  /** Pointer to the underlying DSP core. */
  dsp_engine_core_t* core;
  /** Spectrum analyzer instance. */
  spectrum_analyzer_t* spectrum;
  /** History buffer for captured audio. */
  audio_history_buffer_t* capture_buffer;
  /** History buffer for playback audio. */
  audio_history_buffer_t* playback_buffer;
  /** Target volumes for faders. */
  double desired_fader_volumes[FADER_COUNT];
  /** Target mute states for faders. */
  bool desired_fader_mutes[FADER_COUNT];
  /** Reason for the last processing stop. */
  processing_stop_reason_t last_stop_reason;
  /** True if last stop reason is valid. */
  bool has_last_stop_reason;
  /** Mutex for protecting state variables. */
  pthread_mutex_t state_mutex;
  /** Path to the active configuration file. */
  char active_config_path[1024];
  /** True if active config path is set. */
  bool has_active_config_path;
  /** Path to the state persistence file. */
  char state_file_path[1024];
  /** True if state file path is set. */
  bool has_state_file_path;
  /** True if there are unsaved state changes. */
  bool unsaved_state_changes;
  /** JSON representation of the active configuration. */
  char* active_config_json;
  /** JSON representation of the previous configuration. */
  char* previous_config_json;
  /** Interface function pointer table. */
  dsp_engine_interface_t iface;
};

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "Logging/app_logger.h"
#include "Pipeline/config_loader.h"
#include "Pipeline/state_file.h"

static bool dsp_engine_check_stop_requested(
    dsp_engine_t* engine, processing_stop_reason_t* out_reason);
static const char* dsp_engine_get_state_file(const dsp_engine_t* engine);
static bool dsp_engine_is_state_dirty(const dsp_engine_t* engine);
static void dsp_engine_set_state_dirty(dsp_engine_t* engine, bool dirty);
static char* dsp_engine_get_config_path(const dsp_engine_t* engine);

/**
 * @brief Callback triggered when a chunk is captured by the audio engine core.
 * Appends the chunk to the capture history buffer.
 *
 * @param ctx Pointer to the audio_history_buffer_t capture buffer.
 * @param chunk Pointer to the captured audio_chunk_t.
 */
static void engine_on_chunk_captured_callback(void* ctx,
                                              const audio_chunk_t* chunk) {
  audio_history_buffer_t* buf = (audio_history_buffer_t*)ctx;
  if (buf && chunk) audio_history_buffer_append(buf, chunk);
}

/**
 * @brief Callback triggered when a chunk is processed (played back) by the
 * audio engine core. Appends the chunk to the playback history buffer.
 *
 * @param ctx Pointer to the audio_history_buffer_t playback buffer.
 * @param chunk Pointer to the processed audio_chunk_t.
 */
static void engine_on_chunk_processed_callback(void* ctx,
                                               const audio_chunk_t* chunk) {
  audio_history_buffer_t* buf = (audio_history_buffer_t*)ctx;
  if (buf && chunk) audio_history_buffer_append(buf, chunk);
}

dsp_engine_t* dsp_engine_create(void) {
  dsp_engine_t* engine = (dsp_engine_t*)calloc(1, sizeof(dsp_engine_t));
  if (!engine) return NULL;

  engine->spectrum = spectrum_analyzer_create();
  engine->capture_buffer = audio_history_buffer_create();
  engine->playback_buffer = audio_history_buffer_create();

  if (!engine->spectrum || !engine->capture_buffer || !engine->playback_buffer) {
    dsp_engine_free(engine);
    return NULL;
  }

  for (int i = 0; i < FADER_COUNT; i++) {
    engine->desired_fader_volumes[i] = 0.0;
    engine->desired_fader_mutes[i] = false;
  }
  engine->has_last_stop_reason = false;

  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&engine->state_mutex, &attr);
  pthread_mutexattr_destroy(&attr);
  engine->active_config_path[0] = '\0';
  engine->has_active_config_path = false;
  engine->state_file_path[0] = '\0';
  engine->has_state_file_path = false;
  engine->unsaved_state_changes = false;
  engine->active_config_json = NULL;
  engine->previous_config_json = NULL;

  return engine;
}

void dsp_engine_free(dsp_engine_t* engine) {
  if (!engine) return;
  dsp_engine_stop(engine);
  if (engine->spectrum) spectrum_analyzer_free(engine->spectrum);
  if (engine->capture_buffer) audio_history_buffer_free(engine->capture_buffer);
  if (engine->playback_buffer)
    audio_history_buffer_free(engine->playback_buffer);
  pthread_mutex_destroy(&engine->state_mutex);
  if (engine->active_config_json) free(engine->active_config_json);
  if (engine->previous_config_json) free(engine->previous_config_json);
  free(engine);
}

static bool dsp_engine_set_config_struct_locked(dsp_engine_t* engine,
                                                dsp_config_t* config,
                                                audio_backend_error_t* err);

bool dsp_engine_set_config_struct(dsp_engine_t* engine, dsp_config_t* config,
                                  audio_backend_error_t* err) {
  if (!engine) return false;
  pthread_mutex_lock(&engine->state_mutex);
  bool res = dsp_engine_set_config_struct_locked(engine, config, err);
  pthread_mutex_unlock(&engine->state_mutex);
  return res;
}

static bool dsp_engine_set_config_struct_locked(dsp_engine_t* engine,
                                                dsp_config_t* config,
                                                audio_backend_error_t* err) {
  if (!engine || !config) return false;
  if (engine->core) {
    dsp_engine_core_collect_garbage(engine->core);
  }

  // 1. Hot Reload attempt:
  // If the engine core is currently running, and the new configuration's device
  // properties (e.g. backend, sample rate, channels, buffer sizes) match the
  // running configuration, we can reload the filters/routing pipeline
  // dynamically without tearing down audio threads.
  if (engine->core &&
      dsp_engine_core_get_state(engine->core) != PROCESSING_STATE_INACTIVE) {
    if (devices_config_equal(&engine->core->current_config->devices, &config->devices)) {
      audio_backend_error_t berr;
      if (dsp_engine_core_reload_config(engine->core, config, &berr)) {
        return true;
      } else {
        // Hot reload failed. Stop and clean up the core before failing.
        dsp_engine_core_stop(
            engine->core, (processing_stop_reason_t){.type = STOP_REASON_NONE});
        dsp_engine_core_free(engine->core);
        engine->core = NULL;
        if (err) *err = berr;
        return false;
      }
    }
  }

  // 2. Cold Reload:
  // If hot reload is not possible (device configuration changed or core is not
  // running), we must stop and destroy the existing core, which shuts down the
  // capture/playback loops.
  if (engine->core &&
      dsp_engine_core_get_state(engine->core) != PROCESSING_STATE_INACTIVE) {
    dsp_engine_core_stop(engine->core,
                         (processing_stop_reason_t){.type = STOP_REASON_NONE});
  }
  if (engine->core) {
    dsp_engine_core_free(engine->core);
    engine->core = NULL;
  }

  // Create the new engine core with the new configuration
  dsp_engine_core_t* core = dsp_engine_core_create(config);
  if (!core) {
    dsp_config_free(config);
    if (err) {
      err->type = AUDIO_BACKEND_ERR_COMMAND_SEND;
      snprintf(err->message, sizeof(err->message), "Failed to create core");
    }
    return false;
  }

  // Persist fader levels and mute states across config change
  for (int i = 0; i < FADER_COUNT; i++) {
    double vol = engine->desired_fader_volumes[i];
    bool mute = engine->desired_fader_mutes[i];
    processing_parameters_set_target_volume_for_fader(core->processing_params,
                                                      vol, (fader_t)i);
    processing_parameters_set_current_volume_for_fader(core->processing_params,
                                                       vol, (fader_t)i);
    processing_parameters_set_muted_for_fader(core->processing_params, mute,
                                              (fader_t)i);
  }

  // Resize history buffers to match the new channel counts
  audio_history_buffer_reset(
      engine->capture_buffer,
      capture_device_config_get_channels(&config->devices.capture));
  audio_history_buffer_reset(
      engine->playback_buffer,
      playback_device_config_get_channels(&config->devices.playback));

  // Connect callbacks to feed captured/playback data chunks into history
  // buffers
  core->on_chunk_captured = engine_on_chunk_captured_callback;
  core->on_chunk_captured_ctx = engine->capture_buffer;
  core->on_chunk_processed = engine_on_chunk_processed_callback;
  core->on_chunk_processed_ctx = engine->playback_buffer;

  // Start the audio engine core (spawns capture & playback loops/threads)
  audio_backend_error_t start_err;
  if (!dsp_engine_core_start(core, &start_err)) {
    dsp_engine_core_free(core);
    if (err) *err = start_err;
    return false;
  }

  engine->core = core;
  engine->has_last_stop_reason = false;
  return true;
}

static bool dsp_engine_set_config_locked(dsp_engine_t* engine, const char* json,
                                         audio_backend_error_t* err);

bool dsp_engine_set_config(dsp_engine_t* engine, const char* json,
                           audio_backend_error_t* err) {
  if (!engine) return false;
  pthread_mutex_lock(&engine->state_mutex);
  bool res = dsp_engine_set_config_locked(engine, json, err);
  pthread_mutex_unlock(&engine->state_mutex);
  return res;
}

static bool dsp_engine_set_config_locked(dsp_engine_t* engine, const char* json,
                                         audio_backend_error_t* err) {
  if (!engine || !json) return false;
  logger_t logger = logger_create("dsp.engine");
  static _Thread_local char s_json_log_buf[32768];
  snprintf(s_json_log_buf, sizeof(s_json_log_buf), "%s", json);
  logger_info(&logger, "Set config: %s", log_arg_string(s_json_log_buf),
              log_arg_none(), log_arg_none(), log_arg_none());

  dsp_config_t* parsed = NULL;
  config_error_t cerr;
  memset(&cerr, 0, sizeof(cerr));
  if (config_loader_parse(json, &parsed, &cerr) != 0 || !parsed) {
    if (engine->core) {
      dsp_engine_core_stop(
          engine->core, (processing_stop_reason_t){.type = STOP_REASON_NONE});
      dsp_engine_core_free(engine->core);
      engine->core = NULL;
    }
    if (err) {
      err->type = AUDIO_BACKEND_ERR_CONFIG_PARSE;
      strncpy(err->message, cerr.message, sizeof(err->message) - 1);
      err->message[sizeof(err->message) - 1] = '\0';
    }
    return false;
  }
  bool success = dsp_engine_set_config_struct(engine, parsed, err);
  if (success) {
    pthread_mutex_lock(&engine->state_mutex);
    if (engine->previous_config_json) {
      free(engine->previous_config_json);
    }
    engine->previous_config_json = engine->active_config_json;
    engine->active_config_json = strdup(json);
    pthread_mutex_unlock(&engine->state_mutex);
  }
  return success;
}

void dsp_engine_stop(dsp_engine_t* engine) {
  if (!engine) return;
  pthread_mutex_lock(&engine->state_mutex);
  if (engine->core) {
    dsp_engine_core_collect_garbage(engine->core);
  }
  if (engine->core &&
      dsp_engine_core_get_state(engine->core) != PROCESSING_STATE_INACTIVE) {
    processing_stop_reason_t reason = {.type = STOP_REASON_NONE};
    if (engine->core->shared) {
      reason = engine->core->shared->stop_reason;
    }
    dsp_engine_core_stop(engine->core, reason);
    engine->last_stop_reason = reason;
    engine->has_last_stop_reason = true;
  }
  if (engine->core) {
    dsp_engine_core_free(engine->core);
    engine->core = NULL;
  }
  pthread_mutex_unlock(&engine->state_mutex);
}

void dsp_engine_set_fader_volume(dsp_engine_t* engine, fader_t fader, float db,
                                 bool instant) {
  if (!engine || fader < 0 || fader >= FADER_COUNT) return;
  pthread_mutex_lock(&engine->state_mutex);
  engine->desired_fader_volumes[fader] = (double)db;
  engine->unsaved_state_changes = true;

  if (engine->core && engine->core->processing_params) {
    processing_parameters_set_target_volume_for_fader(
        engine->core->processing_params, (double)db, fader);
    if (instant) {
      processing_parameters_set_current_volume_for_fader(
          engine->core->processing_params, (double)db, fader);
    }
  }
  pthread_mutex_unlock(&engine->state_mutex);
}

void dsp_engine_set_fader_mute(dsp_engine_t* engine, fader_t fader, bool mute) {
  if (!engine || fader < 0 || fader >= FADER_COUNT) return;
  pthread_mutex_lock(&engine->state_mutex);
  engine->desired_fader_mutes[fader] = mute;
  engine->unsaved_state_changes = true;

  if (engine->core && engine->core->processing_params) {
    processing_parameters_set_muted_for_fader(engine->core->processing_params,
                                              mute, fader);
  }
  pthread_mutex_unlock(&engine->state_mutex);
}

float dsp_engine_get_fader_volume(const dsp_engine_t* engine, fader_t fader) {
  if (!engine || fader < 0 || fader >= FADER_COUNT) return 0.0f;
  pthread_mutex_lock((pthread_mutex_t*)&engine->state_mutex);
  float db = (float)engine->desired_fader_volumes[fader];
  pthread_mutex_unlock((pthread_mutex_t*)&engine->state_mutex);
  return db;
}

bool dsp_engine_is_fader_muted(const dsp_engine_t* engine, fader_t fader) {
  if (!engine || fader < 0 || fader >= FADER_COUNT) return false;
  pthread_mutex_lock((pthread_mutex_t*)&engine->state_mutex);
  bool mute = engine->desired_fader_mutes[fader];
  pthread_mutex_unlock((pthread_mutex_t*)&engine->state_mutex);
  return mute;
}

static state_update_t dsp_engine_get_status_locked(const dsp_engine_t* engine);

state_update_t dsp_engine_get_status(const dsp_engine_t* engine) {
  if (!engine) {
    state_update_t res;
    memset(&res, 0, sizeof(res));
    res.state = PROCESSING_STATE_INACTIVE;
    res.stop_reason.type = STOP_REASON_NONE;
    return res;
  }
  pthread_mutex_lock((pthread_mutex_t*)&engine->state_mutex);
  state_update_t res = dsp_engine_get_status_locked(engine);
  pthread_mutex_unlock((pthread_mutex_t*)&engine->state_mutex);
  return res;
}

static state_update_t dsp_engine_get_status_locked(const dsp_engine_t* engine) {
  state_update_t res;
  memset(&res, 0, sizeof(res));
  if (engine->core) {
    dsp_engine_core_collect_garbage((dsp_engine_core_t*)engine->core);
    res.state = dsp_engine_core_get_state(engine->core);
    const processing_stop_reason_t* r =
        dsp_engine_core_get_stop_reason(engine->core);
    if (r && r->type != STOP_REASON_NONE) {
      res.stop_reason = *r;
    } else if (engine->has_last_stop_reason) {
      res.stop_reason = engine->last_stop_reason;
    } else {
      res.stop_reason.type = STOP_REASON_NONE;
    }
  } else {
    res.state = PROCESSING_STATE_INACTIVE;
    if (engine->has_last_stop_reason) {
      res.stop_reason = engine->last_stop_reason;
    } else {
      res.stop_reason.type = STOP_REASON_NONE;
    }
  }
  return res;
}

static vu_levels_t dsp_engine_get_vu_levels_locked(const dsp_engine_t* engine);

vu_levels_t dsp_engine_get_vu_levels(const dsp_engine_t* engine) {
  if (!engine) {
    vu_levels_t res;
    memset(&res, 0, sizeof(res));
    return res;
  }
  pthread_mutex_lock((pthread_mutex_t*)&engine->state_mutex);
  vu_levels_t res = dsp_engine_get_vu_levels_locked(engine);
  pthread_mutex_unlock((pthread_mutex_t*)&engine->state_mutex);
  return res;
}

static vu_levels_t dsp_engine_get_vu_levels_locked(const dsp_engine_t* engine) {
  vu_levels_t res;
  memset(&res, 0, sizeof(res));
  if (!engine->core || !engine->core->processing_params) return res;
  dsp_engine_core_collect_garbage((dsp_engine_core_t*)engine->core);
  processing_parameters_t* p = engine->core->processing_params;
  res.playback_channels = p->playback_channels;
  res.capture_channels = p->capture_channels;
  if (res.playback_channels > 0) {
    res.playback_rms = (double*)calloc(res.playback_channels, sizeof(double));
    res.playback_peak = (double*)calloc(res.playback_channels, sizeof(double));
    if (res.playback_rms)
      processing_parameters_get_playback_signal_rms(p, res.playback_rms,
                                                    res.playback_channels);
    if (res.playback_peak)
      processing_parameters_get_playback_signal_peak(p, res.playback_peak,
                                                     res.playback_channels);
  }
  if (res.capture_channels > 0) {
    res.capture_rms = (double*)calloc(res.capture_channels, sizeof(double));
    res.capture_peak = (double*)calloc(res.capture_channels, sizeof(double));
    if (res.capture_rms)
      processing_parameters_get_capture_signal_rms(p, res.capture_rms,
                                                   res.capture_channels);
    if (res.capture_peak)
      processing_parameters_get_capture_signal_peak(p, res.capture_peak,
                                                    res.capture_channels);
  }
  return res;
}

void dsp_engine_free_vu_levels(vu_levels_t* levels) {
  if (!levels) return;
  free(levels->playback_rms);
  free(levels->playback_peak);
  free(levels->capture_rms);
  free(levels->capture_peak);
  memset(levels, 0, sizeof(vu_levels_t));
}

static spectrum_status_t dsp_engine_get_spectrum_locked(
    dsp_engine_t* engine, bool is_capture, int channel, double min_freq,
    double max_freq, size_t n_bins, spectrum_result_t* out_result);

spectrum_status_t dsp_engine_get_spectrum(dsp_engine_t* engine, bool is_capture,
                                          int channel, double min_freq,
                                          double max_freq, size_t n_bins,
                                          spectrum_result_t* out_result) {
  if (!engine) return SPECTRUM_ERROR_EMPTY;
  pthread_mutex_lock(&engine->state_mutex);
  spectrum_status_t res = dsp_engine_get_spectrum_locked(
      engine, is_capture, channel, min_freq, max_freq, n_bins, out_result);
  pthread_mutex_unlock(&engine->state_mutex);
  return res;
}

static spectrum_status_t dsp_engine_get_spectrum_locked(
    dsp_engine_t* engine, bool is_capture, int channel, double min_freq,
    double max_freq, size_t n_bins, spectrum_result_t* out_result) {
  if (!engine->core || !engine->spectrum) return SPECTRUM_ERROR_EMPTY;
  audio_history_buffer_t* buf =
      is_capture ? engine->capture_buffer : engine->playback_buffer;
  size_t samplerate = engine->core->current_config->devices.samplerate;
  return spectrum_analyzer_compute(engine->spectrum, buf, channel, min_freq,
                                   max_freq, n_bins, samplerate, out_result);
}

static audio_samples_t* dsp_engine_get_samples_locked(
    dsp_engine_t* engine, bool is_capture, size_t n_frames,
    audio_backend_error_t* err);

audio_samples_t* dsp_engine_get_samples(dsp_engine_t* engine, bool is_capture,
                                        size_t n_frames,
                                        audio_backend_error_t* err) {
  if (!engine) return NULL;
  pthread_mutex_lock(&engine->state_mutex);
  audio_samples_t* res =
      dsp_engine_get_samples_locked(engine, is_capture, n_frames, err);
  pthread_mutex_unlock(&engine->state_mutex);
  return res;
}

static audio_samples_t* dsp_engine_get_samples_locked(
    dsp_engine_t* engine, bool is_capture, size_t n_frames,
    audio_backend_error_t* err) {
  if (!engine->core) {
    if (err) {
      err->type = AUDIO_BACKEND_ERR_ENGINE_NOT_RUNNING;
      snprintf(err->message, sizeof(err->message), "Engine not running");
    }
    return NULL;
  }
  audio_history_buffer_t* buf =
      is_capture ? engine->capture_buffer : engine->playback_buffer;
  if (!audio_history_buffer_has_data(buf)) {
    if (err) {
      err->type = AUDIO_BACKEND_ERR_BUFFER_EMPTY;
      snprintf(err->message, sizeof(err->message), "Buffer empty");
    }
    return NULL;
  }

  size_t n = n_frames;
  if (n > AUDIO_HISTORY_BUFFER_CAPACITY) n = AUDIO_HISTORY_BUFFER_CAPACITY;
  size_t ch_count = audio_history_buffer_get_channels(buf);
  if (ch_count == 0) {
    if (err) {
      err->type = AUDIO_BACKEND_ERR_BUFFER_EMPTY;
      snprintf(err->message, sizeof(err->message), "No channels");
    }
    return NULL;
  }

  audio_samples_t* res = (audio_samples_t*)calloc(1, sizeof(audio_samples_t));
  if (!res) return NULL;
  res->channels_count = ch_count;
  res->frames = n;
  res->channels = (double**)calloc(ch_count, sizeof(double*));
  if (!res->channels) {
    free(res);
    return NULL;
  }

  float* tmp = (float*)calloc(n, sizeof(float));
  for (size_t ch = 0; ch < ch_count; ch++) {
    res->channels[ch] = (double*)calloc(n, sizeof(double));
    bool enough = false;
    audio_history_buffer_status_t status =
        audio_history_buffer_read_latest(buf, tmp, n, (int)ch, &enough);
    if (status != AUDIO_HISTORY_BUFFER_OK) {
      free(tmp);
      dsp_engine_free_samples(res);
      if (err) {
        err->type = AUDIO_BACKEND_ERR_BUFFER_EMPTY;
        snprintf(err->message, sizeof(err->message), "Failed to read buffer");
      }
      return NULL;
    }
    for (size_t i = 0; i < n; i++) {
      res->channels[ch][i] = (double)tmp[i];
    }
  }
  free(tmp);
  return res;
}

void dsp_engine_free_samples(audio_samples_t* samples) {
  if (!samples) return;
  if (samples->channels) {
    for (size_t ch = 0; ch < samples->channels_count; ch++) {
      free(samples->channels[ch]);
    }
    free(samples->channels);
  }
  free(samples);
}

void dsp_engine_set_log_level(log_level_t level) {
  app_logger_set_level(level);
}

int dsp_engine_get_available_devices(const char* backend, bool input,
                                     audio_device_t* out_devices,
                                     int max_devices) {
  if (!backend) return 0;
  if (strcasecmp(backend, "coreaudio") == 0) {
#if defined(ENABLE_COREAUDIO)
    char names[32][256];
    int count =
        core_audio_capabilities_available_device_names(input, names, 32);
    if (count > max_devices) count = max_devices;
    for (int i = 0; i < count; i++) {
      if (out_devices) {
        memcpy(out_devices[i].name, names[i], sizeof(out_devices[i].name));
        out_devices[i].name[sizeof(out_devices[i].name) - 1] = '\0';
      }
    }
    return count;
#else
    return 0;
#endif
  } else if (strcasecmp(backend, "alsa") == 0) {
#if defined(ENABLE_ALSA)
    char names[32][256];
    int count = alsa_capabilities_available_device_names(input, names, 32);
    if (count > max_devices) count = max_devices;
    for (int i = 0; i < count; i++) {
      if (out_devices) {
        memcpy(out_devices[i].name, names[i], sizeof(out_devices[i].name));
        out_devices[i].name[sizeof(out_devices[i].name) - 1] = '\0';
      }
    }
    return count;
#else
    return 0;
#endif
  } else if (strcasecmp(backend, "wasapi") == 0) {
#if defined(ENABLE_WASAPI)
    char names[32][256];
    int count = wasapi_capabilities_available_device_names(input, names, 32);
    if (count > max_devices) count = max_devices;
    for (int i = 0; i < count; i++) {
      if (out_devices) {
        memcpy(out_devices[i].name, names[i], sizeof(out_devices[i].name));
        out_devices[i].name[sizeof(out_devices[i].name) - 1] = '\0';
      }
    }
    return count;
#else
    return 0;
#endif
  } else if (strcasecmp(backend, "asio") == 0) {
#if defined(ENABLE_ASIO)
    char names[32][256];
    int count = asio_capabilities_available_device_names(input, names, 32);
    if (count > max_devices) count = max_devices;
    for (int i = 0; i < count; i++) {
      if (out_devices) {
        memcpy(out_devices[i].name, names[i], sizeof(out_devices[i].name));
        out_devices[i].name[sizeof(out_devices[i].name) - 1] = '\0';
      }
    }
    return count;
#else
    return 0;
#endif
  }
  return 0;
}

audio_device_descriptor_t* dsp_engine_get_device_capabilities(
    const char* backend, const char* device, bool is_capture,
    device_error_t* err) {
  if (!backend || !device) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER,
                        "Invalid backend or device name");
    }
    return NULL;
  }
  if (strcasecmp(backend, "coreaudio") == 0) {
#if defined(ENABLE_COREAUDIO)
    return core_audio_capabilities_describe(device, is_capture, err);
#else
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER,
                        "CoreAudio backend not compiled");
    }
    return NULL;
#endif
  } else if (strcasecmp(backend, "alsa") == 0) {
#if defined(ENABLE_ALSA)
    return alsa_capabilities_describe(device, is_capture, err);
#else
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER, "ALSA backend not compiled");
    }
    return NULL;
#endif
  } else if (strcasecmp(backend, "wasapi") == 0) {
#if defined(ENABLE_WASAPI)
    return wasapi_capabilities_describe(device, is_capture, err);
#else
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER, "WASAPI backend not compiled");
    }
    return NULL;
#endif
  } else if (strcasecmp(backend, "asio") == 0) {
#if defined(ENABLE_ASIO)
    return asio_capabilities_describe(device, is_capture, err);
#else
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER, "ASIO backend not compiled");
    }
    return NULL;
#endif
  }
  if (err) {
    device_error_init(err, DEVICE_ERROR_OTHER, "Unsupported backend");
  }
  return NULL;
}

void dsp_engine_free_device_capabilities(audio_device_descriptor_t* desc) {
  free_audio_device_descriptor(desc);
}

const dsp_config_t* dsp_engine_get_active_config(const dsp_engine_t* engine) {
  if (!engine) return NULL;
  pthread_mutex_lock((pthread_mutex_t*)&engine->state_mutex);
  const dsp_config_t* res = engine->core ? engine->core->current_config : NULL;
  pthread_mutex_unlock((pthread_mutex_t*)&engine->state_mutex);
  return res;
}

processing_parameters_t* dsp_engine_get_processing_parameters(
    const dsp_engine_t* engine) {
  if (!engine) return NULL;
  pthread_mutex_lock((pthread_mutex_t*)&engine->state_mutex);
  processing_parameters_t* res =
      engine->core ? engine->core->processing_params : NULL;
  pthread_mutex_unlock((pthread_mutex_t*)&engine->state_mutex);
  return res;
}

static bool iface_get_status(void* ctx, state_update_t* out_status) {
  if (!ctx || !out_status) return false;
  *out_status = dsp_engine_get_status((dsp_engine_t*)ctx);
  return true;
}
static bool iface_get_processing_parameters(void* ctx, void** out_params) {
  if (!ctx || !out_params) return false;
  *out_params = (void*)dsp_engine_get_processing_parameters((dsp_engine_t*)ctx);
  return *out_params != NULL;
}
static bool iface_get_active_config_json(void* ctx, char** out_json) {
  if (!ctx || !out_json) return false;
  dsp_engine_t* engine = (dsp_engine_t*)ctx;
  pthread_mutex_lock(&engine->state_mutex);
  if (engine->active_config_json) {
    *out_json = strdup(engine->active_config_json);
    pthread_mutex_unlock(&engine->state_mutex);
    return true;
  }
  pthread_mutex_unlock(&engine->state_mutex);
  *out_json = NULL;
  return false;
}
static bool iface_get_previous_config_json(void* ctx, char** out_json) {
  if (!ctx || !out_json) return false;
  dsp_engine_t* engine = (dsp_engine_t*)ctx;
  pthread_mutex_lock(&engine->state_mutex);
  if (engine->previous_config_json) {
    *out_json = strdup(engine->previous_config_json);
    pthread_mutex_unlock(&engine->state_mutex);
    return true;
  }
  pthread_mutex_unlock(&engine->state_mutex);
  *out_json = NULL;
  return false;
}
static bool iface_get_vu_levels(void* ctx, vu_levels_t* out_vu) {
  if (!ctx || !out_vu) return false;
  *out_vu = dsp_engine_get_vu_levels((dsp_engine_t*)ctx);
  return true;
}
static bool iface_get_available_devices(void* ctx, const char* backend,
                                        bool is_input,
                                        audio_device_t** out_devices,
                                        size_t* out_count) {
  if (!ctx || !out_devices || !out_count) return false;
  static _Thread_local audio_device_t devs[32];
  int n = dsp_engine_get_available_devices(backend, is_input, devs, 32);
  *out_devices = devs;
  *out_count = (size_t)n;
  return true;
}
static bool iface_get_device_capabilities(void* ctx, const char* backend,
                                          const char* device, bool is_capture,
                                          audio_device_descriptor_t** out_desc,
                                          device_error_t* out_err) {
  if (!ctx || !out_desc) return false;
  *out_desc =
      dsp_engine_get_device_capabilities(backend, device, is_capture, out_err);
  return *out_desc != NULL;
}
static bool iface_get_spectrum(void* ctx, bool is_capture, uint32_t channel,
                               double min_freq, double max_freq,
                               uint32_t n_bins, spectrum_t* out_spec) {
  if (!ctx || !out_spec) return false;
  spectrum_result_t res;
  if (dsp_engine_get_spectrum((dsp_engine_t*)ctx, is_capture, (int)channel,
                              min_freq, max_freq, (size_t)n_bins, &res) != 0)
    return false;
  out_spec->count = res.count;
  out_spec->frequencies = (double*)calloc(res.count, sizeof(double));
  out_spec->magnitudes = (double*)calloc(res.count, sizeof(double));
  if (!out_spec->frequencies || !out_spec->magnitudes) {
    if (out_spec->frequencies) free(out_spec->frequencies);
    if (out_spec->magnitudes) free(out_spec->magnitudes);
    out_spec->frequencies = NULL;
    out_spec->magnitudes = NULL;
    out_spec->count = 0;
    return false;
  }
  for (size_t i = 0; i < res.count; i++) {
    out_spec->frequencies[i] = (double)res.frequencies[i];
    out_spec->magnitudes[i] = (double)res.magnitudes[i];
  }
  return true;
}
static bool iface_set_config_json(void* ctx, const char* json_str,
                                  audio_backend_error_t* out_err) {
  if (!ctx) return false;
  return dsp_engine_set_config((dsp_engine_t*)ctx, json_str, out_err);
}
static void iface_stop(void* ctx) {
  if (ctx) dsp_engine_stop((dsp_engine_t*)ctx);
}
static const dsp_config_t* iface_get_active_config(void* ctx) {
  if (!ctx) return NULL;
  return dsp_engine_get_active_config((dsp_engine_t*)ctx);
}
static void iface_set_fader_volume(void* ctx, fader_t fader, float db,
                                   bool instant) {
  if (ctx) dsp_engine_set_fader_volume((dsp_engine_t*)ctx, fader, db, instant);
}
static void iface_set_fader_mute(void* ctx, fader_t fader, bool mute) {
  if (ctx) dsp_engine_set_fader_mute((dsp_engine_t*)ctx, fader, mute);
}

static const char* iface_get_state_file(void* ctx) {
  return ctx ? dsp_engine_get_state_file((dsp_engine_t*)ctx) : NULL;
}
static bool iface_is_state_dirty(void* ctx) {
  return ctx ? dsp_engine_is_state_dirty((dsp_engine_t*)ctx) : false;
}
static char* iface_get_config_path(void* ctx) {
  return ctx ? dsp_engine_get_config_path((dsp_engine_t*)ctx) : NULL;
}
static void iface_set_config_path(void* ctx, const char* path) {
  if (ctx) dsp_engine_set_config_path((dsp_engine_t*)ctx, path);
}

/**
 * @brief Checks if the running audio threads have requested the engine to stop.
 *
 * Querying the atomic flag `should_stop` from the core's shared state informs
 * the main engine thread that a thread has failed (e.g. buffer
 * underrun/overrun, device disconnected, etc.) and the engine needs to halt
 * processing.
 *
 * @param engine Pointer to the dsp_engine_t instance.
 * @param out_reason Pointer to write the stop reason details into (optional).
 * @return true if a stop has been requested, false otherwise.
 */
static bool dsp_engine_check_stop_requested(
    dsp_engine_t* engine, processing_stop_reason_t* out_reason) {
  if (!engine || !engine->core || !engine->core->shared) return false;
  bool req = atomic_load_explicit(&engine->core->shared->should_stop,
                                  memory_order_acquire);
  if (req && out_reason) {
    *out_reason = engine->core->shared->stop_reason;
  }
  return req;
}

void dsp_engine_set_state_file(dsp_engine_t* engine, const char* path) {
  if (!engine) return;
  pthread_mutex_lock(&engine->state_mutex);
  if (path && path[0]) {
    strncpy(engine->state_file_path, path, sizeof(engine->state_file_path) - 1);
    engine->has_state_file_path = true;
  } else {
    engine->state_file_path[0] = '\0';
    engine->has_state_file_path = false;
  }
  pthread_mutex_unlock(&engine->state_mutex);
}

/**
 * @brief Thread-safe getter for the state file path.
 *
 * @param engine Pointer to the dsp_engine_t instance.
 * @return The state file path string, or NULL if none set.
 */
static const char* dsp_engine_get_state_file(const dsp_engine_t* engine) {
  if (!engine) return NULL;
  pthread_mutex_lock((pthread_mutex_t*)&engine->state_mutex);
  const char* path =
      engine->has_state_file_path ? engine->state_file_path : NULL;
  pthread_mutex_unlock((pthread_mutex_t*)&engine->state_mutex);
  return path;
}

/**
 * @brief Thread-safe check to see if there are unsaved state changes (dirty
 * flag).
 *
 * @param engine Pointer to the dsp_engine_t instance.
 * @return true if there are unsaved fader/config path updates, false otherwise.
 */
static bool dsp_engine_is_state_dirty(const dsp_engine_t* engine) {
  if (!engine) return false;
  pthread_mutex_lock((pthread_mutex_t*)&engine->state_mutex);
  bool dirty = engine->unsaved_state_changes;
  pthread_mutex_unlock((pthread_mutex_t*)&engine->state_mutex);
  return dirty;
}

/**
 * @brief Thread-safe setter for the unsaved changes dirty flag.
 *
 * @param engine Pointer to the dsp_engine_t instance.
 * @param dirty Whether the engine has unsaved changes.
 */
static void dsp_engine_set_state_dirty(dsp_engine_t* engine, bool dirty) {
  if (!engine) return;
  pthread_mutex_lock(&engine->state_mutex);
  engine->unsaved_state_changes = dirty;
  pthread_mutex_unlock(&engine->state_mutex);
}

void dsp_engine_set_config_path(dsp_engine_t* engine, const char* path) {
  if (!engine) return;
  pthread_mutex_lock(&engine->state_mutex);
  if (path && path[0]) {
    strncpy(engine->active_config_path, path,
            sizeof(engine->active_config_path) - 1);
    engine->has_active_config_path = true;
  } else {
    engine->active_config_path[0] = '\0';
    engine->has_active_config_path = false;
  }
  engine->unsaved_state_changes = true;
  pthread_mutex_unlock(&engine->state_mutex);
}

/**
 * @brief Thread-safe getter for the active configuration file path.
 *
 * @param engine Pointer to the dsp_engine_t instance.
 * @return An allocated copy of the active config path, or NULL. Must be freed
 * by caller.
 */
static char* dsp_engine_get_config_path(const dsp_engine_t* engine) {
  if (!engine) return NULL;
  pthread_mutex_lock((pthread_mutex_t*)&engine->state_mutex);
  char* path = NULL;
  if (engine->has_active_config_path) {
    path = strdup(engine->active_config_path);
  }
  pthread_mutex_unlock((pthread_mutex_t*)&engine->state_mutex);
  return path;
}

void dsp_engine_poll(dsp_engine_t* engine) {
  if (!engine) return;

  pthread_mutex_lock(&engine->state_mutex);
  if (engine->core) {
    dsp_engine_core_collect_garbage(engine->core);
  }
  pthread_mutex_unlock(&engine->state_mutex);

  // 1. Process asynchronous stop requests from the loop threads (e.g. ALSA
  // errors). If the background thread encountered a fatal error and requested a
  // stop, we stop and free the core resources here on the main controller
  // thread.
  pthread_mutex_lock(&engine->state_mutex);
  processing_stop_reason_t stop_reason;
  if (dsp_engine_check_stop_requested(engine, &stop_reason)) {
    dsp_engine_stop(engine);
  }
  pthread_mutex_unlock(&engine->state_mutex);

  // 2. State persistence serialization:
  // If any fader positions or configuration path values have changed since the
  // last poll, serialize them to the configured state file so the state is
  // restored on engine restart.
  pthread_mutex_lock(&engine->state_mutex);
  bool dirty = engine->unsaved_state_changes;
  bool has_state = engine->has_state_file_path;
  pthread_mutex_unlock(&engine->state_mutex);

  if (has_state && dirty) {
    dsp_state_t* state_to_save = dsp_state_create();
    if (state_to_save) {
      char* current_path = dsp_engine_get_config_path(engine);
      if (current_path) {
        if (current_path[0]) {
          dsp_state_set_config_path(state_to_save, current_path);
        }
        free(current_path);
      }

      for (int i = 0; i < FADER_COUNT; i++) {
        dsp_state_set_volume(state_to_save, i,
                             dsp_engine_get_fader_volume(engine, (fader_t)i));
        dsp_state_set_mute(state_to_save, i,
                           dsp_engine_is_fader_muted(engine, (fader_t)i));
      }

      const char* s_path = dsp_engine_get_state_file(engine);
      if (s_path && dsp_state_save(s_path, state_to_save)) {
        dsp_engine_set_state_dirty(engine, false);
      }
      dsp_state_free(state_to_save);
    }
  }
}

dsp_engine_interface_t* dsp_engine_get_interface(dsp_engine_t* engine) {
  if (!engine) return NULL;
  engine->iface.ctx = engine;
  engine->iface.get_status = iface_get_status;
  engine->iface.get_processing_parameters = iface_get_processing_parameters;
  engine->iface.get_active_config_json = iface_get_active_config_json;
  engine->iface.get_previous_config_json = iface_get_previous_config_json;
  engine->iface.get_active_config = iface_get_active_config;
  engine->iface.get_vu_levels = iface_get_vu_levels;
  engine->iface.get_available_devices = iface_get_available_devices;
  engine->iface.get_device_capabilities = iface_get_device_capabilities;
  engine->iface.get_spectrum = iface_get_spectrum;
  engine->iface.set_config_json = iface_set_config_json;
  engine->iface.stop = iface_stop;
  engine->iface.set_fader_volume = iface_set_fader_volume;
  engine->iface.set_fader_mute = iface_set_fader_mute;
  engine->iface.get_state_file = iface_get_state_file;
  engine->iface.is_state_dirty = iface_is_state_dirty;
  engine->iface.get_config_path = iface_get_config_path;
  engine->iface.set_config_path = iface_set_config_path;
  return &engine->iface;
}
