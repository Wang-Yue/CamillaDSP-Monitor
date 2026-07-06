#include "dsp_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "Logging/app_logger.h"
#include "Pipeline/config_loader.h"

static void engine_on_chunk_captured_callback(void* ctx,
                                              const audio_chunk_t* chunk) {
  audio_history_buffer_t* buf = (audio_history_buffer_t*)ctx;
  if (buf && chunk) audio_history_buffer_append(buf, chunk);
}

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

  for (int i = 0; i < FADER_COUNT; i++) {
    engine->desired_fader_volumes[i] = 0.0;
    engine->desired_fader_mutes[i] = false;
  }
  engine->has_last_stop_reason = false;

  return engine;
}

void dsp_engine_free(dsp_engine_t* engine) {
  if (!engine) return;
  dsp_engine_stop(engine);
  if (engine->spectrum) spectrum_analyzer_free(engine->spectrum);
  if (engine->capture_buffer) audio_history_buffer_free(engine->capture_buffer);
  if (engine->playback_buffer)
    audio_history_buffer_free(engine->playback_buffer);
  free(engine);
}

bool dsp_engine_set_config(dsp_engine_t* engine, const char* json,
                           audio_backend_error_t* err) {
  if (!engine || !json) return false;
  logger_t logger = logger_create("dsp.engine");
  logger_info(&logger, "Set config: %s", log_arg_string(json), log_arg_none(),
              log_arg_none(), log_arg_none());

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

  if (engine->core &&
      dsp_engine_core_get_state(engine->core) != PROCESSING_STATE_INACTIVE) {
    if (memcmp(&engine->core->current_config->devices, &parsed->devices,
               sizeof(devices_config_t)) == 0) {
      audio_backend_error_t berr;
      if (dsp_engine_core_reload_config(engine->core, parsed, &berr)) {
        return true;
      } else {
        dsp_engine_core_stop(
            engine->core, (processing_stop_reason_t){.type = STOP_REASON_NONE});
        dsp_engine_core_free(engine->core);
        engine->core = NULL;
        if (err) *err = berr;
        return false;
      }
    }
  }

  if (engine->core &&
      dsp_engine_core_get_state(engine->core) != PROCESSING_STATE_INACTIVE) {
    dsp_engine_core_stop(engine->core,
                         (processing_stop_reason_t){.type = STOP_REASON_NONE});
  }
  if (engine->core) {
    dsp_engine_core_free(engine->core);
    engine->core = NULL;
  }

  dsp_engine_core_t* core = dsp_engine_core_create(parsed);
  if (!core) {
    dsp_config_free(parsed);
    if (err) {
      err->type = AUDIO_BACKEND_ERR_COMMAND_SEND;
      snprintf(err->message, sizeof(err->message), "Failed to create core");
    }
    return false;
  }

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

  audio_history_buffer_reset(engine->capture_buffer,
                             parsed->devices.capture.channels);
  audio_history_buffer_reset(engine->playback_buffer,
                             parsed->devices.playback.channels);

  core->on_chunk_captured = engine_on_chunk_captured_callback;
  core->on_chunk_captured_ctx = engine->capture_buffer;
  core->on_chunk_processed = engine_on_chunk_processed_callback;
  core->on_chunk_processed_ctx = engine->playback_buffer;

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

void dsp_engine_stop(dsp_engine_t* engine) {
  if (!engine) return;
  if (engine->core &&
      dsp_engine_core_get_state(engine->core) != PROCESSING_STATE_INACTIVE) {
    dsp_engine_core_stop(engine->core,
                         (processing_stop_reason_t){.type = STOP_REASON_NONE});
    engine->last_stop_reason =
        (processing_stop_reason_t){.type = STOP_REASON_NONE};
    engine->has_last_stop_reason = true;
  }
  if (engine->core) {
    dsp_engine_core_free(engine->core);
    engine->core = NULL;
  }
}

void dsp_engine_set_fader_volume(dsp_engine_t* engine, fader_t fader,
                                 float db) {
  if (!engine || fader < 0 || fader >= FADER_COUNT) return;
  engine->desired_fader_volumes[fader] = (double)db;
  if (engine->core && engine->core->processing_params) {
    processing_parameters_set_target_volume_for_fader(
        engine->core->processing_params, (double)db, fader);
  }
}

void dsp_engine_set_fader_mute(dsp_engine_t* engine, fader_t fader, bool mute) {
  if (!engine || fader < 0 || fader >= FADER_COUNT) return;
  engine->desired_fader_mutes[fader] = mute;
  if (engine->core && engine->core->processing_params) {
    processing_parameters_set_muted_for_fader(engine->core->processing_params,
                                              mute, fader);
  }
}

float dsp_engine_get_fader_volume(const dsp_engine_t* engine, fader_t fader) {
  if (!engine || fader < 0 || fader >= FADER_COUNT) return 0.0f;
  return (float)engine->desired_fader_volumes[fader];
}

bool dsp_engine_is_fader_muted(const dsp_engine_t* engine, fader_t fader) {
  if (!engine || fader < 0 || fader >= FADER_COUNT) return false;
  return engine->desired_fader_mutes[fader];
}

state_update_t dsp_engine_get_status(const dsp_engine_t* engine) {
  state_update_t res;
  memset(&res, 0, sizeof(res));
  if (!engine) {
    res.state = PROCESSING_STATE_INACTIVE;
    res.stop_reason.type = STOP_REASON_NONE;
    return res;
  }
  if (engine->core) {
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

vu_levels_t dsp_engine_get_vu_levels(const dsp_engine_t* engine) {
  vu_levels_t res;
  memset(&res, 0, sizeof(res));
  if (!engine || !engine->core || !engine->core->processing_params) return res;
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

spectrum_status_t dsp_engine_get_spectrum(dsp_engine_t* engine, bool is_capture,
                                          int channel, double min_freq,
                                          double max_freq, size_t n_bins,
                                          spectrum_result_t* out_result) {
  if (!engine || !engine->core || !engine->spectrum)
    return SPECTRUM_ERROR_EMPTY;
  audio_history_buffer_t* buf =
      is_capture ? engine->capture_buffer : engine->playback_buffer;
  size_t samplerate = engine->core->current_config->devices.samplerate;
  return spectrum_analyzer_compute(engine->spectrum, buf, channel, min_freq,
                                   max_freq, n_bins, samplerate, out_result);
}

audio_samples_t* dsp_engine_get_samples(dsp_engine_t* engine, bool is_capture,
                                        size_t n_frames,
                                        audio_backend_error_t* err) {
  if (!engine || !engine->core) {
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
  size_t ch_count = buf->channels;
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
#ifdef __APPLE__
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
#if defined(__linux__)
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
#if defined(_WIN32)
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
#if defined(_WIN32)
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
    const char* backend, const char* device, bool is_capture) {
  if (!backend || !device) return NULL;
  if (strcasecmp(backend, "coreaudio") == 0) {
#ifdef __APPLE__
    return core_audio_capabilities_describe(device, is_capture);
#else
    return NULL;
#endif
  } else if (strcasecmp(backend, "alsa") == 0) {
#if defined(__linux__)
    return alsa_capabilities_describe(device, is_capture);
#else
    return NULL;
#endif
  } else if (strcasecmp(backend, "wasapi") == 0) {
#if defined(_WIN32)
    return wasapi_capabilities_describe(device, is_capture);
#else
    return NULL;
#endif
  } else if (strcasecmp(backend, "asio") == 0) {
#if defined(_WIN32)
    return asio_capabilities_describe(device, is_capture);
#else
    return NULL;
#endif
  }
  return NULL;
}

void dsp_engine_free_device_capabilities(audio_device_descriptor_t* desc) {
  if (!desc) return;
#if defined(__APPLE__)
  core_audio_capabilities_free_descriptor(desc);
#elif defined(__linux__)
  alsa_capabilities_free_descriptor(desc);
#elif defined(_WIN32)
  // We can check the description's name or we can free both cleanly,
  // but calling the correct free function is safer.
  // Let's check name matching or just check if it was allocated by WASAPI /
  // ASIO. Both WASAPI and ASIO capability descriptors use identical nested
  // structure allocations, so wasapi_capabilities_free_descriptor and
  // asio_capabilities_free_descriptor do exactly the same nested free loop. We
  // can call either one! But let's call the correct one based on name/driver if
  // possible, or just call wasapi's or asio's. Let's call
  // asio_capabilities_free_descriptor as it is fully equivalent.
  asio_capabilities_free_descriptor(desc);
#endif
}

const dsp_config_t* dsp_engine_get_active_config(const dsp_engine_t* engine) {
  return engine && engine->core ? engine->core->current_config : NULL;
}

processing_parameters_t* dsp_engine_get_processing_parameters(
    const dsp_engine_t* engine) {
  return engine && engine->core ? engine->core->processing_params : NULL;
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
  static audio_device_t devs[32];
  int n = dsp_engine_get_available_devices(backend, is_input, devs, 32);
  *out_devices = devs;
  *out_count = (size_t)n;
  return true;
}
static bool iface_get_device_capabilities(
    void* ctx, const char* backend, const char* device, bool is_capture,
    audio_device_descriptor_t** out_desc) {
  if (!ctx || !out_desc) return false;
  *out_desc = dsp_engine_get_device_capabilities(backend, device, is_capture);
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
  for (size_t i = 0; i < res.count; i++) {
    if (out_spec->frequencies)
      out_spec->frequencies[i] = (double)res.frequencies[i];
    if (out_spec->magnitudes)
      out_spec->magnitudes[i] = (double)res.magnitudes[i];
  }
  return true;
}
static bool iface_set_config_json(void* ctx, const char* json_str,
                                  char* out_err_msg, size_t err_len) {
  if (!ctx) return false;
  audio_backend_error_t err;
  bool ok = dsp_engine_set_config((dsp_engine_t*)ctx, json_str, &err);
  if (!ok && out_err_msg && err_len > 0) {
    snprintf(out_err_msg, err_len, "%s", err.message);
  }
  return ok;
}
static void iface_stop(void* ctx) {
  if (ctx) dsp_engine_stop((dsp_engine_t*)ctx);
}
static const dsp_config_t* iface_get_active_config(void* ctx) {
  if (!ctx) return NULL;
  return dsp_engine_get_active_config((dsp_engine_t*)ctx);
}

dsp_engine_interface_t* dsp_engine_get_interface(dsp_engine_t* engine) {
  if (!engine) return NULL;
  static dsp_engine_interface_t iface;
  iface.ctx = engine;
  iface.get_status = iface_get_status;
  iface.get_processing_parameters = iface_get_processing_parameters;
  iface.get_active_config_json = iface_get_active_config_json;
  iface.get_active_config = iface_get_active_config;
  iface.get_vu_levels = iface_get_vu_levels;
  iface.get_available_devices = iface_get_available_devices;
  iface.get_device_capabilities = iface_get_device_capabilities;
  iface.get_spectrum = iface_get_spectrum;
  iface.set_config_json = iface_set_config_json;
  iface.stop = iface_stop;
  return &iface;
}
