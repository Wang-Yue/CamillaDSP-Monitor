#if defined(__linux__)
#define _GNU_SOURCE
#endif
#define JSMN_STATIC
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "configuration.h"
#include "jsmn.h"

static void get_tok_string(const char* js, const jsmntok_t* tok, char* dest,
                           size_t dest_len) {
  int len = tok->end - tok->start;
  if (len >= (int)dest_len) len = (int)dest_len - 1;
  memcpy(dest, js + tok->start, len);
  dest[len] = '\0';
}

static double get_tok_double(const char* js, const jsmntok_t* tok) {
  if (!tok) return 0.0;
  char buf[64];
  get_tok_string(js, tok, buf, sizeof(buf));
  return strtod(buf, NULL);
}

static int get_tok_int(const char* js, const jsmntok_t* tok) {
  if (!tok) return 0;
  char buf[64];
  get_tok_string(js, tok, buf, sizeof(buf));
  return (int)strtol(buf, NULL, 10);
}

static bool get_tok_bool(const char* js, const jsmntok_t* tok) {
  if (!tok) return false;
  int len = tok->end - tok->start;
  if (len == 4 && strncmp(js + tok->start, "true", 4) == 0) return true;
  return false;
}

static int skip_token(const jsmntok_t* tokens, int start_idx) {
  int i = start_idx;
  int remaining = 1;
  while (remaining > 0) {
    int children = tokens[i].size;
    remaining += children - 1;
    i++;
  }
  return i;
}

static int find_object_key(const char* js, const jsmntok_t* tokens, int count,
                           int obj_idx, const char* key) {
  if (obj_idx < 0 || obj_idx >= count || tokens[obj_idx].type != JSMN_OBJECT)
    return -1;
  int size = tokens[obj_idx].size;
  int i = obj_idx + 1;
  for (int k = 0; k < size; k++) {
    if (i >= count) return -1;
    if (tokens[i].type == JSMN_STRING) {
      int len = tokens[i].end - tokens[i].start;
      if (len == (int)strlen(key) &&
          strncmp(js + tokens[i].start, key, len) == 0) {
        return i + 1;  // return index of the value token
      }
    }
    // skip key and its value
    i = skip_token(tokens, i);
  }
  return -1;
}

static int find_top_level_key(const char* js, const jsmntok_t* tokens,
                              int count, const char* key) {
  if (count <= 0 || tokens[0].type != JSMN_OBJECT) return -1;
  int size = tokens[0].size;
  int i = 1;
  for (int k = 0; k < size; k++) {
    if (i >= count) return -1;
    if (tokens[i].type == JSMN_STRING) {
      int len = tokens[i].end - tokens[i].start;
      if (len == (int)strlen(key) &&
          strncmp(js + tokens[i].start, key, len) == 0) {
        return i + 1;  // Return the token index of the value
      }
    }
    // skip key and its value
    i = skip_token(tokens, i);
  }
  return -1;
}

static int get_array_element(const jsmntok_t* tokens, int count, int arr_idx,
                             int element_idx) {
  if (arr_idx < 0 || arr_idx >= count || tokens[arr_idx].type != JSMN_ARRAY)
    return -1;
  int size = tokens[arr_idx].size;
  if (element_idx < 0 || element_idx >= size) return -1;
  int i = arr_idx + 1;
  for (int k = 0; k < element_idx; k++) {
    i = skip_token(tokens, i);
  }
  return i;
}

static void parse_resampler(const char* js, const jsmntok_t* tokens, int count,
                            int res_val_idx, devices_config_t* devices) {
  if (res_val_idx == -1 || tokens[res_val_idx].type != JSMN_OBJECT) return;
  resampler_config_t* res = &devices->resampler;
  devices->has_resampler = true;

  int type_idx = find_object_key(js, tokens, count, res_val_idx, "type");
  if (type_idx != -1 && tokens[type_idx].type == JSMN_STRING) {
    char type_str[64];
    get_tok_string(js, &tokens[type_idx], type_str, sizeof(type_str));
    res->type = resampler_type_from_string(type_str);
  }

  int prof_idx = find_object_key(js, tokens, count, res_val_idx, "profile");
  if (prof_idx != -1 && tokens[prof_idx].type == JSMN_STRING) {
    get_tok_string(js, &tokens[prof_idx], res->profile, sizeof(res->profile));
    res->has_profile = true;
  }

  int interp_idx =
      find_object_key(js, tokens, count, res_val_idx, "interpolation");
  if (interp_idx != -1 && tokens[interp_idx].type == JSMN_STRING) {
    get_tok_string(js, &tokens[interp_idx], res->interpolation,
                   sizeof(res->interpolation));
    res->has_interpolation = true;
  }

#if defined(__APPLE__)
  int aq_idx = find_object_key(js, tokens, count, res_val_idx, "apple_quality");
  if (aq_idx != -1 && tokens[aq_idx].type == JSMN_STRING) {
    char aq_str[64];
    get_tok_string(js, &tokens[aq_idx], aq_str, sizeof(aq_str));
    res->apple_quality = apple_resampler_quality_from_string(aq_str);
    res->has_apple_quality = true;
  }
  int ac_idx =
      find_object_key(js, tokens, count, res_val_idx, "apple_complexity");
  if (ac_idx != -1 && tokens[ac_idx].type == JSMN_STRING) {
    char ac_str[64];
    get_tok_string(js, &tokens[ac_idx], ac_str, sizeof(ac_str));
    res->apple_complexity = apple_resampler_complexity_from_string(ac_str);
    res->has_apple_complexity = true;
  }
#endif

  int sl_idx = find_object_key(js, tokens, count, res_val_idx, "sinc_len");
  if (sl_idx != -1 && tokens[sl_idx].type == JSMN_PRIMITIVE) {
    res->sinc_len = get_tok_int(js, &tokens[sl_idx]);
    res->has_sinc_len = (res->sinc_len > 0);
  }

  int of_idx =
      find_object_key(js, tokens, count, res_val_idx, "oversampling_factor");
  if (of_idx != -1 && tokens[of_idx].type == JSMN_PRIMITIVE) {
    res->oversampling_factor = get_tok_int(js, &tokens[of_idx]);
    res->has_oversampling_factor = (res->oversampling_factor > 0);
  }

  int win_idx = find_object_key(js, tokens, count, res_val_idx, "window");
  if (win_idx != -1 && tokens[win_idx].type == JSMN_STRING) {
    get_tok_string(js, &tokens[win_idx], res->window, sizeof(res->window));
    res->has_window = true;
  }

  int fc_idx = find_object_key(js, tokens, count, res_val_idx, "f_cutoff");
  if (fc_idx != -1 && tokens[fc_idx].type == JSMN_PRIMITIVE) {
    res->f_cutoff = get_tok_double(js, &tokens[fc_idx]);
    res->has_f_cutoff = (res->f_cutoff > 0.0);
  }
}

static void parse_capture(const char* js, const jsmntok_t* tokens, int count,
                          int cap_val_idx, devices_config_t* devices) {
  if (cap_val_idx == -1 || tokens[cap_val_idx].type != JSMN_OBJECT) return;
  capture_device_config_t* cap = &devices->capture;

  int ch_idx = find_object_key(js, tokens, count, cap_val_idx, "channels");
  if (ch_idx != -1 && tokens[ch_idx].type == JSMN_PRIMITIVE) {
    cap->channels = get_tok_int(js, &tokens[ch_idx]);
  }

  int type_idx = find_object_key(js, tokens, count, cap_val_idx, "type");
  char type_str[64] = "";
  if (type_idx != -1 && tokens[type_idx].type == JSMN_STRING) {
    get_tok_string(js, &tokens[type_idx], type_str, sizeof(type_str));
    cap->type = audio_backend_type_from_string(type_str);
    if (strcasecmp(type_str, "WavFile") == 0) {
      cap->is_wav = true;
      cap->has_is_wav = true;
    }
  } else {
#if defined(__APPLE__)
    cap->type = AUDIO_BACKEND_TYPE_CORE_AUDIO;
#elif defined(__linux__)
    cap->type = AUDIO_BACKEND_TYPE_ALSA;
#elif defined(_WIN32)
    cap->type = AUDIO_BACKEND_TYPE_WASAPI;
#endif
  }

  int dev_idx = find_object_key(js, tokens, count, cap_val_idx, "device");
  if (dev_idx != -1 && tokens[dev_idx].type == JSMN_STRING) {
    get_tok_string(js, &tokens[dev_idx], cap->device, sizeof(cap->device));
    cap->has_device = true;
  }

  int fn_idx = find_object_key(js, tokens, count, cap_val_idx, "filename");
  if (fn_idx != -1 && tokens[fn_idx].type == JSMN_STRING) {
    get_tok_string(js, &tokens[fn_idx], cap->filename, sizeof(cap->filename));
    cap->has_filename = true;
  }

  int fmt_idx = find_object_key(js, tokens, count, cap_val_idx, "format");
  if (fmt_idx != -1 && tokens[fmt_idx].type == JSMN_STRING) {
    char fmt_str[64];
    get_tok_string(js, &tokens[fmt_idx], fmt_str, sizeof(fmt_str));
    if (cap->type == AUDIO_BACKEND_TYPE_FILE ||
        cap->type == AUDIO_BACKEND_TYPE_STDIN_OUT) {
      cap->file_format = binary_sample_format_from_string(fmt_str);
      cap->has_file_format = true;
#if defined(_WIN32)
    } else if (cap->type == AUDIO_BACKEND_TYPE_WASAPI) {
      cap->format = wasapi_sample_format_from_string(fmt_str);
      cap->has_format = true;
    } else if (cap->type == AUDIO_BACKEND_TYPE_ASIO) {
      cap->asio_format = asio_sample_format_from_string(fmt_str);
      cap->has_asio_format = true;
#endif
    } else {
#if defined(__linux__)
      cap->format = alsa_sample_format_from_string(fmt_str);
#elif defined(__APPLE__)
      cap->format = coreaudio_sample_format_from_string(fmt_str);
#endif
      cap->has_format = true;
    }
  }

#if defined(_WIN32)
  int lb_idx = find_object_key(js, tokens, count, cap_val_idx, "loopback");
  if (lb_idx != -1 && tokens[lb_idx].type == JSMN_PRIMITIVE) {
    cap->loopback = get_tok_bool(js, &tokens[lb_idx]);
    cap->has_loopback = true;
  }
  int ex_idx = find_object_key(js, tokens, count, cap_val_idx, "exclusive");
  if (ex_idx != -1 && tokens[ex_idx].type == JSMN_PRIMITIVE) {
    cap->exclusive = get_tok_bool(js, &tokens[ex_idx]);
    cap->has_exclusive = true;
  }
#endif

  int sb_idx = find_object_key(js, tokens, count, cap_val_idx, "skip_bytes");
  if (sb_idx != -1 && tokens[sb_idx].type == JSMN_PRIMITIVE) {
    cap->skip_bytes = get_tok_int(js, &tokens[sb_idx]);
    cap->has_skip_bytes = (cap->skip_bytes > 0);
  }

  int rb_idx = find_object_key(js, tokens, count, cap_val_idx, "read_bytes");
  if (rb_idx != -1 && tokens[rb_idx].type == JSMN_PRIMITIVE) {
    cap->read_bytes = get_tok_int(js, &tokens[rb_idx]);
    cap->has_read_bytes = (cap->read_bytes > 0);
  }

  int es_idx = find_object_key(js, tokens, count, cap_val_idx, "extra_samples");
  if (es_idx != -1 && tokens[es_idx].type == JSMN_PRIMITIVE) {
    cap->extra_samples = get_tok_int(js, &tokens[es_idx]);
    cap->has_extra_samples = (cap->extra_samples > 0);
  }

#if defined(__linux__)
  int soi_idx =
      find_object_key(js, tokens, count, cap_val_idx, "stop_on_inactive");
  if (soi_idx != -1 && tokens[soi_idx].type == JSMN_PRIMITIVE) {
    cap->stop_on_inactive = get_tok_bool(js, &tokens[soi_idx]);
    cap->has_stop_on_inactive = true;
  }
  int lvc_idx =
      find_object_key(js, tokens, count, cap_val_idx, "link_volume_control");
  if (lvc_idx != -1 && tokens[lvc_idx].type == JSMN_STRING) {
    get_tok_string(js, &tokens[lvc_idx], cap->link_volume_control,
                   sizeof(cap->link_volume_control));
    cap->has_link_volume_control = true;
  }
  int lmc_idx =
      find_object_key(js, tokens, count, cap_val_idx, "link_mute_control");
  if (lmc_idx != -1 && tokens[lmc_idx].type == JSMN_STRING) {
    get_tok_string(js, &tokens[lmc_idx], cap->link_mute_control,
                   sizeof(cap->link_mute_control));
    cap->has_link_mute_control = true;
  }
#endif

  int bd_idx = find_object_key(js, tokens, count, cap_val_idx, "bypass_dop");
  if (bd_idx != -1 && tokens[bd_idx].type == JSMN_PRIMITIVE) {
    cap->bypass_dop = get_tok_bool(js, &tokens[bd_idx]);
    cap->has_bypass_dop = true;
  }

  int dc_idx = find_object_key(js, tokens, count, cap_val_idx, "dop_cutoff_hz");
  if (dc_idx != -1 && tokens[dc_idx].type == JSMN_PRIMITIVE) {
    cap->dop_cutoff_hz = get_tok_double(js, &tokens[dc_idx]);
    cap->has_dop_cutoff_hz = true;
  }

  int sig_idx = find_object_key(js, tokens, count, cap_val_idx, "signal");
  if (sig_idx != -1 && tokens[sig_idx].type == JSMN_OBJECT) {
    int sig_type_idx = find_object_key(js, tokens, count, sig_idx, "type");
    if (sig_type_idx != -1 && tokens[sig_type_idx].type == JSMN_STRING) {
      char sig_type_str[64];
      get_tok_string(js, &tokens[sig_type_idx], sig_type_str,
                     sizeof(sig_type_str));
      cap->generator.type = signal_type_from_string(sig_type_str);
    } else {
      cap->generator.type = SIGNAL_TYPE_SINE;
    }
    int freq_idx = find_object_key(js, tokens, count, sig_idx, "freq");
    if (freq_idx != -1 && tokens[freq_idx].type == JSMN_PRIMITIVE) {
      cap->generator.frequency = get_tok_double(js, &tokens[freq_idx]);
    } else {
      cap->generator.frequency = 1000.0;
    }
    int lev_idx = find_object_key(js, tokens, count, sig_idx, "level");
    if (lev_idx != -1 && tokens[lev_idx].type == JSMN_PRIMITIVE) {
      cap->generator.level = get_tok_double(js, &tokens[lev_idx]);
    } else {
      cap->generator.level = 0.0;
    }
    cap->has_generator = true;
  }
}

static void parse_playback(const char* js, const jsmntok_t* tokens, int count,
                           int play_val_idx, devices_config_t* devices) {
  if (play_val_idx == -1 || tokens[play_val_idx].type != JSMN_OBJECT) return;
  playback_device_config_t* play = &devices->playback;

  int ch_idx = find_object_key(js, tokens, count, play_val_idx, "channels");
  if (ch_idx != -1 && tokens[ch_idx].type == JSMN_PRIMITIVE) {
    play->channels = get_tok_int(js, &tokens[ch_idx]);
  }

  int type_idx = find_object_key(js, tokens, count, play_val_idx, "type");
  if (type_idx != -1 && tokens[type_idx].type == JSMN_STRING) {
    char type_str[64];
    get_tok_string(js, &tokens[type_idx], type_str, sizeof(type_str));
    play->type = audio_backend_type_from_string(type_str);
  } else {
#if defined(__APPLE__)
    play->type = AUDIO_BACKEND_TYPE_CORE_AUDIO;
#elif defined(__linux__)
    play->type = AUDIO_BACKEND_TYPE_ALSA;
#elif defined(_WIN32)
    play->type = AUDIO_BACKEND_TYPE_WASAPI;
#endif
  }

  int dev_idx = find_object_key(js, tokens, count, play_val_idx, "device");
  if (dev_idx != -1 && tokens[dev_idx].type == JSMN_STRING) {
    get_tok_string(js, &tokens[dev_idx], play->device, sizeof(play->device));
    play->has_device = true;
  }

  int fn_idx = find_object_key(js, tokens, count, play_val_idx, "filename");
  if (fn_idx != -1 && tokens[fn_idx].type == JSMN_STRING) {
    get_tok_string(js, &tokens[fn_idx], play->filename, sizeof(play->filename));
    play->has_filename = true;
  }

  int fmt_idx = find_object_key(js, tokens, count, play_val_idx, "format");
  if (fmt_idx != -1 && tokens[fmt_idx].type == JSMN_STRING) {
    char fmt_str[64];
    get_tok_string(js, &tokens[fmt_idx], fmt_str, sizeof(fmt_str));
    if (play->type == AUDIO_BACKEND_TYPE_FILE ||
        play->type == AUDIO_BACKEND_TYPE_STDIN_OUT) {
      play->file_format = binary_sample_format_from_string(fmt_str);
      play->has_file_format = true;
#if defined(_WIN32)
    } else if (play->type == AUDIO_BACKEND_TYPE_WASAPI) {
      play->format = wasapi_sample_format_from_string(fmt_str);
      play->has_format = true;
    } else if (play->type == AUDIO_BACKEND_TYPE_ASIO) {
      play->asio_format = asio_sample_format_from_string(fmt_str);
      play->has_asio_format = true;
#endif
    } else {
#if defined(__linux__)
      play->format = alsa_sample_format_from_string(fmt_str);
#elif defined(__APPLE__)
      play->format = coreaudio_sample_format_from_string(fmt_str);
#endif
      play->has_format = true;
    }
  }

  int wav_idx = find_object_key(js, tokens, count, play_val_idx, "wav_header");
  if (wav_idx != -1 && tokens[wav_idx].type == JSMN_PRIMITIVE) {
    play->is_wav = get_tok_bool(js, &tokens[wav_idx]);
    play->has_is_wav = true;
  }

  int ex_idx = find_object_key(js, tokens, count, play_val_idx, "exclusive");
  if (ex_idx != -1 && tokens[ex_idx].type == JSMN_PRIMITIVE) {
    play->exclusive = get_tok_bool(js, &tokens[ex_idx]);
    play->has_exclusive = true;
  }

  int od_idx = find_object_key(js, tokens, count, play_val_idx, "output_dop");
  if (od_idx != -1 && tokens[od_idx].type == JSMN_PRIMITIVE) {
    play->output_dop = get_tok_bool(js, &tokens[od_idx]);
    play->has_output_dop = true;
  }
}

static int parse_devices(const char* js, const jsmntok_t* tokens, int count,
                         int dev_val_idx, dsp_config_t* config,
                         config_error_t* err) {
  if (tokens[dev_val_idx].type != JSMN_OBJECT) {
    config_error_set(err, CONFIG_ERR_PARSE, "devices must be an object");
    return -1;
  }
  devices_config_t* dev = &config->devices;

  int sr_idx = find_object_key(js, tokens, count, dev_val_idx, "samplerate");
  if (sr_idx != -1 && tokens[sr_idx].type == JSMN_PRIMITIVE) {
    int parsed_sr = get_tok_int(js, &tokens[sr_idx]);
    dev->samplerate = parsed_sr > 0 ? (size_t)parsed_sr : 0;
  }

  int cs_idx = find_object_key(js, tokens, count, dev_val_idx, "chunksize");
  if (cs_idx != -1 && tokens[cs_idx].type == JSMN_PRIMITIVE) {
    int parsed_cs = get_tok_int(js, &tokens[cs_idx]);
    dev->chunksize = parsed_cs > 0 ? (size_t)parsed_cs : 0;
  }

  int ql_idx = find_object_key(js, tokens, count, dev_val_idx, "queuelimit");
  if (ql_idx != -1 && tokens[ql_idx].type == JSMN_PRIMITIVE) {
    dev->queuelimit = get_tok_int(js, &tokens[ql_idx]);
    dev->has_queuelimit = (dev->queuelimit > 0);
  }

  int era_idx =
      find_object_key(js, tokens, count, dev_val_idx, "enable_rate_adjust");
  if (era_idx != -1 && tokens[era_idx].type == JSMN_PRIMITIVE) {
    dev->enable_rate_adjust = get_tok_bool(js, &tokens[era_idx]);
    dev->has_enable_rate_adjust = true;
  }

  int tl_idx = find_object_key(js, tokens, count, dev_val_idx, "target_level");
  if (tl_idx != -1 && tokens[tl_idx].type == JSMN_PRIMITIVE) {
    dev->target_level = get_tok_int(js, &tokens[tl_idx]);
    dev->has_target_level = (dev->target_level > 0);
  }

  int ap_idx = find_object_key(js, tokens, count, dev_val_idx, "adjust_period");
  if (ap_idx != -1 && tokens[ap_idx].type == JSMN_PRIMITIVE) {
    dev->adjust_period = get_tok_double(js, &tokens[ap_idx]);
    dev->has_adjust_period = (dev->adjust_period > 0.0);
  }

  int st_idx =
      find_object_key(js, tokens, count, dev_val_idx, "silence_threshold");
  if (st_idx != -1 && tokens[st_idx].type == JSMN_PRIMITIVE) {
    dev->silence_threshold = get_tok_double(js, &tokens[st_idx]);
    dev->has_silence_threshold = (dev->silence_threshold != 0.0);
  }

  int sto_idx =
      find_object_key(js, tokens, count, dev_val_idx, "silence_timeout");
  if (sto_idx != -1 && tokens[sto_idx].type == JSMN_PRIMITIVE) {
    dev->silence_timeout = get_tok_double(js, &tokens[sto_idx]);
    dev->has_silence_timeout = (dev->silence_timeout > 0.0);
  }

  int csr_idx =
      find_object_key(js, tokens, count, dev_val_idx, "capture_samplerate");
  if (csr_idx != -1 && tokens[csr_idx].type == JSMN_PRIMITIVE) {
    int parsed_cap_sr = get_tok_int(js, &tokens[csr_idx]);
    dev->capture_samplerate = parsed_cap_sr > 0 ? (size_t)parsed_cap_sr : 0;
    dev->has_capture_samplerate = (parsed_cap_sr > 0);
  }

  int vrt_idx =
      find_object_key(js, tokens, count, dev_val_idx, "volume_ramp_time");
  if (vrt_idx != -1 && tokens[vrt_idx].type == JSMN_PRIMITIVE) {
    dev->volume_ramp_time = get_tok_double(js, &tokens[vrt_idx]);
    dev->has_volume_ramp_time = (dev->volume_ramp_time > 0.0);
  }

  int vl_idx = find_object_key(js, tokens, count, dev_val_idx, "volume_limit");
  if (vl_idx != -1 && tokens[vl_idx].type == JSMN_PRIMITIVE) {
    dev->volume_limit = get_tok_double(js, &tokens[vl_idx]);
    dev->has_volume_limit = (dev->volume_limit > 0.0);
  }

  int sor_idx =
      find_object_key(js, tokens, count, dev_val_idx, "stop_on_rate_change");
  if (sor_idx != -1 && tokens[sor_idx].type == JSMN_PRIMITIVE) {
    dev->stop_on_rate_change = get_tok_bool(js, &tokens[sor_idx]);
    dev->has_stop_on_rate_change = true;
  }

  int rmi_idx =
      find_object_key(js, tokens, count, dev_val_idx, "rate_measure_interval");
  if (rmi_idx != -1 && tokens[rmi_idx].type == JSMN_PRIMITIVE) {
    dev->rate_measure_interval = get_tok_double(js, &tokens[rmi_idx]);
    dev->has_rate_measure_interval = (dev->rate_measure_interval > 0.0);
  }

  int mt_idx = find_object_key(js, tokens, count, dev_val_idx, "multithreaded");
  if (mt_idx != -1 && tokens[mt_idx].type == JSMN_PRIMITIVE) {
    dev->multithreaded = get_tok_bool(js, &tokens[mt_idx]);
    dev->has_multithreaded = true;
  }

  int wt_idx =
      find_object_key(js, tokens, count, dev_val_idx, "worker_threads");
  if (wt_idx != -1 && tokens[wt_idx].type == JSMN_PRIMITIVE) {
    dev->worker_threads = get_tok_int(js, &tokens[wt_idx]);
    dev->has_worker_threads = (dev->worker_threads > 0);
  }

  parse_resampler(js, tokens, count,
                  find_object_key(js, tokens, count, dev_val_idx, "resampler"),
                  dev);
  parse_capture(js, tokens, count,
                find_object_key(js, tokens, count, dev_val_idx, "capture"),
                dev);
  parse_playback(js, tokens, count,
                 find_object_key(js, tokens, count, dev_val_idx, "playback"),
                 dev);

  return 0;
}

static int parse_pipeline(const char* js, const jsmntok_t* tokens, int count,
                          int pipe_val_idx, dsp_config_t* config,
                          config_error_t* err) {
  if (tokens[pipe_val_idx].type != JSMN_ARRAY) {
    config_error_set(err, CONFIG_ERR_PARSE, "pipeline must be an array");
    return -1;
  }
  int size = tokens[pipe_val_idx].size;
  if (size == 0) return 0;

  config->pipeline = (pipeline_step_t*)calloc(size, sizeof(pipeline_step_t));
  if (!config->pipeline) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return -1;
  }
  config->pipeline_count = size;

  int step_idx = pipe_val_idx + 1;
  for (int s = 0; s < size; s++) {
    if (tokens[step_idx].type != JSMN_OBJECT) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Pipeline step must be an object");
      return -1;
    }
    pipeline_step_t* step = &config->pipeline[s];

    int type_idx = find_object_key(js, tokens, count, step_idx, "type");
    if (type_idx != -1 && tokens[type_idx].type == JSMN_STRING) {
      char type_str[64];
      get_tok_string(js, &tokens[type_idx], type_str, sizeof(type_str));
      if (strcmp(type_str, "Filter") == 0)
        step->type = PIPELINE_STEP_TYPE_FILTER;
      else if (strcmp(type_str, "Mixer") == 0)
        step->type = PIPELINE_STEP_TYPE_MIXER;
      else if (strcmp(type_str, "Processor") == 0)
        step->type = PIPELINE_STEP_TYPE_PROCESSOR;
    }

    int name_idx = find_object_key(js, tokens, count, step_idx, "name");
    if (name_idx != -1 && tokens[name_idx].type == JSMN_STRING) {
      get_tok_string(js, &tokens[name_idx], step->name, sizeof(step->name));
      step->has_name = true;
    }

    int ch_idx = find_object_key(js, tokens, count, step_idx, "channel");
    if (ch_idx != -1 && tokens[ch_idx].type == JSMN_PRIMITIVE) {
      step->channel = get_tok_int(js, &tokens[ch_idx]);
      step->has_channel = true;
    }

    int bypassed_idx = find_object_key(js, tokens, count, step_idx, "bypassed");
    if (bypassed_idx != -1 && tokens[bypassed_idx].type == JSMN_PRIMITIVE) {
      step->bypassed = get_tok_bool(js, &tokens[bypassed_idx]);
    }

    int names_idx = find_object_key(js, tokens, count, step_idx, "names");
    if (names_idx != -1 && tokens[names_idx].type == JSMN_ARRAY) {
      int names_size = tokens[names_idx].size;
      step->names = (char**)calloc(names_size, sizeof(char*));
      step->names_count = names_size;
      for (int n = 0; n < names_size; n++) {
        int el_idx = get_array_element(tokens, count, names_idx, n);
        if (el_idx != -1 && tokens[el_idx].type == JSMN_STRING) {
          char name_buf[128];
          get_tok_string(js, &tokens[el_idx], name_buf, sizeof(name_buf));
          step->names[n] = strdup(name_buf);
        }
      }
    }

    int channels_idx = find_object_key(js, tokens, count, step_idx, "channels");
    if (channels_idx != -1 && tokens[channels_idx].type == JSMN_ARRAY) {
      int ch_size = tokens[channels_idx].size;
      step->channels = (int*)calloc(ch_size, sizeof(int));
      step->channels_count = ch_size;
      for (int c = 0; c < ch_size; c++) {
        int el_idx = get_array_element(tokens, count, channels_idx, c);
        if (el_idx != -1 && tokens[el_idx].type == JSMN_PRIMITIVE) {
          step->channels[c] = get_tok_int(js, &tokens[el_idx]);
        }
      }
    }

    step_idx = skip_token(tokens, step_idx);
  }
  return 0;
}

static int parse_mixers(const char* js, const jsmntok_t* tokens, int count,
                        int mixers_val_idx, dsp_config_t* config,
                        config_error_t* err) {
  if (tokens[mixers_val_idx].type != JSMN_OBJECT) {
    config_error_set(err, CONFIG_ERR_PARSE, "mixers must be an object");
    return -1;
  }
  int size = tokens[mixers_val_idx].size;
  if (size == 0) return 0;

  config->mixers =
      (named_mixer_config_t*)calloc(size, sizeof(named_mixer_config_t));
  if (!config->mixers) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return -1;
  }
  config->mixers_count = size;

  int mixer_key_idx = mixers_val_idx + 1;
  for (int m = 0; m < size; m++) {
    named_mixer_config_t* nm = &config->mixers[m];
    get_tok_string(js, &tokens[mixer_key_idx], nm->name, sizeof(nm->name));

    int mixer_val_idx = mixer_key_idx + 1;
    if (tokens[mixer_val_idx].type != JSMN_OBJECT) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Mixer definition must be an object");
      return -1;
    }

    mixer_config_t* m_conf = &nm->mixer;

    int channels_idx =
        find_object_key(js, tokens, count, mixer_val_idx, "channels");
    if (channels_idx != -1 && tokens[channels_idx].type == JSMN_OBJECT) {
      int in_idx = find_object_key(js, tokens, count, channels_idx, "in");
      if (in_idx != -1 && tokens[in_idx].type == JSMN_PRIMITIVE) {
        m_conf->channels_in = get_tok_int(js, &tokens[in_idx]);
      }
      int out_idx = find_object_key(js, tokens, count, channels_idx, "out");
      if (out_idx != -1 && tokens[out_idx].type == JSMN_PRIMITIVE) {
        m_conf->channels_out = get_tok_int(js, &tokens[out_idx]);
      }
    }

    int mapping_idx =
        find_object_key(js, tokens, count, mixer_val_idx, "mapping");
    if (mapping_idx != -1 && tokens[mapping_idx].type == JSMN_ARRAY) {
      int map_size = tokens[mapping_idx].size;
      m_conf->mapping =
          (mixer_mapping_t*)calloc(map_size, sizeof(mixer_mapping_t));
      m_conf->mapping_count = map_size;

      for (int mp = 0; mp < map_size; mp++) {
        int map_el_idx = get_array_element(tokens, count, mapping_idx, mp);
        if (map_el_idx != -1 && tokens[map_el_idx].type == JSMN_OBJECT) {
          mixer_mapping_t* mapping = &m_conf->mapping[mp];

          int dest_idx = find_object_key(js, tokens, count, map_el_idx, "dest");
          if (dest_idx != -1 && tokens[dest_idx].type == JSMN_PRIMITIVE) {
            mapping->dest = get_tok_int(js, &tokens[dest_idx]);
          }

          int mute_idx = find_object_key(js, tokens, count, map_el_idx, "mute");
          if (mute_idx != -1 && tokens[mute_idx].type == JSMN_PRIMITIVE) {
            mapping->mute = get_tok_bool(js, &tokens[mute_idx]);
          }

          int sources_idx =
              find_object_key(js, tokens, count, map_el_idx, "sources");
          if (sources_idx != -1 && tokens[sources_idx].type == JSMN_ARRAY) {
            int src_size = tokens[sources_idx].size;
            mapping->sources =
                (mixer_source_t*)calloc(src_size, sizeof(mixer_source_t));
            mapping->sources_count = src_size;

            for (int s = 0; s < src_size; s++) {
              int src_el_idx = get_array_element(tokens, count, sources_idx, s);
              if (src_el_idx != -1 && tokens[src_el_idx].type == JSMN_OBJECT) {
                mixer_source_t* src = &mapping->sources[s];

                int chan_idx =
                    find_object_key(js, tokens, count, src_el_idx, "channel");
                if (chan_idx != -1 && tokens[chan_idx].type == JSMN_PRIMITIVE) {
                  src->channel = get_tok_int(js, &tokens[chan_idx]);
                }

                int gain_idx =
                    find_object_key(js, tokens, count, src_el_idx, "gain");
                if (gain_idx != -1 && tokens[gain_idx].type == JSMN_PRIMITIVE) {
                  src->gain = get_tok_double(js, &tokens[gain_idx]);
                  src->has_gain = true;
                }

                int scale_idx =
                    find_object_key(js, tokens, count, src_el_idx, "scale");
                if (scale_idx != -1 && tokens[scale_idx].type == JSMN_STRING) {
                  char scale_str[32];
                  get_tok_string(js, &tokens[scale_idx], scale_str,
                                 sizeof(scale_str));
                  if (strcasecmp(scale_str, "Linear") == 0)
                    src->scale = GAIN_SCALE_LINEAR;
                  else
                    src->scale = GAIN_SCALE_DB;
                } else {
                  src->scale = GAIN_SCALE_DB;
                }

                int inv_idx =
                    find_object_key(js, tokens, count, src_el_idx, "inverted");
                if (inv_idx != -1 && tokens[inv_idx].type == JSMN_PRIMITIVE) {
                  src->inverted = get_tok_bool(js, &tokens[inv_idx]);
                }

                int smute_idx =
                    find_object_key(js, tokens, count, src_el_idx, "mute");
                if (smute_idx != -1 &&
                    tokens[smute_idx].type == JSMN_PRIMITIVE) {
                  src->mute = get_tok_bool(js, &tokens[smute_idx]);
                }
              }
            }
          }
        }
      }
    }

    mixer_key_idx = skip_token(tokens, mixer_val_idx);
  }
  return 0;
}

static int parse_filters(const char* js, const jsmntok_t* tokens, int count,
                         int filters_val_idx, dsp_config_t* config,
                         config_error_t* err) {
  if (tokens[filters_val_idx].type != JSMN_OBJECT) {
    config_error_set(err, CONFIG_ERR_PARSE, "filters must be an object");
    return -1;
  }
  int size = tokens[filters_val_idx].size;
  if (size == 0) return 0;

  config->filters =
      (named_filter_config_t*)calloc(size, sizeof(named_filter_config_t));
  if (!config->filters) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return -1;
  }
  config->filters_count = size;

  int filter_key_idx = filters_val_idx + 1;
  for (int f = 0; f < size; f++) {
    named_filter_config_t* nf = &config->filters[f];
    get_tok_string(js, &tokens[filter_key_idx], nf->name, sizeof(nf->name));

    int filter_val_idx = filter_key_idx + 1;
    if (tokens[filter_val_idx].type != JSMN_OBJECT) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Filter definition must be an object");
      return -1;
    }

    filter_config_t* f_conf = &nf->filter;

    int type_idx = find_object_key(js, tokens, count, filter_val_idx, "type");
    if (type_idx != -1 && tokens[type_idx].type == JSMN_STRING) {
      char type_str[64];
      get_tok_string(js, &tokens[type_idx], type_str, sizeof(type_str));
      f_conf->type = filter_type_from_string(type_str);
    }

    int params_idx =
        find_object_key(js, tokens, count, filter_val_idx, "parameters");
    if (params_idx != -1 && tokens[params_idx].type == JSMN_OBJECT) {
      switch (f_conf->type) {
        case FILTER_TYPE_GAIN: {
          gain_parameters_t* gp = &f_conf->parameters.gain;
          int gain_idx = find_object_key(js, tokens, count, params_idx, "gain");
          if (gain_idx != -1 && tokens[gain_idx].type == JSMN_PRIMITIVE) {
            gp->gain = get_tok_double(js, &tokens[gain_idx]);
            gp->has_gain = true;
          }
          int scale_idx =
              find_object_key(js, tokens, count, params_idx, "scale");
          if (scale_idx != -1 && tokens[scale_idx].type == JSMN_STRING) {
            char scale_str[32];
            get_tok_string(js, &tokens[scale_idx], scale_str,
                           sizeof(scale_str));
            if (strcasecmp(scale_str, "Linear") == 0)
              gp->scale = GAIN_SCALE_LINEAR;
            else
              gp->scale = GAIN_SCALE_DB;
          } else {
            gp->scale = GAIN_SCALE_DB;
          }
          int inv_idx =
              find_object_key(js, tokens, count, params_idx, "inverted");
          if (inv_idx != -1 && tokens[inv_idx].type == JSMN_PRIMITIVE) {
            gp->inverted = get_tok_bool(js, &tokens[inv_idx]);
          }
          int mute_idx = find_object_key(js, tokens, count, params_idx, "mute");
          if (mute_idx != -1 && tokens[mute_idx].type == JSMN_PRIMITIVE) {
            gp->mute = get_tok_bool(js, &tokens[mute_idx]);
          }
          break;
        }
        case FILTER_TYPE_VOLUME: {
          volume_parameters_t* vp = &f_conf->parameters.volume;
          int rt_idx =
              find_object_key(js, tokens, count, params_idx, "ramp_time");
          if (rt_idx != -1 && tokens[rt_idx].type == JSMN_PRIMITIVE) {
            vp->ramp_time = get_tok_double(js, &tokens[rt_idx]);
            vp->has_ramp_time = true;
          }
          int lim_idx = find_object_key(js, tokens, count, params_idx, "limit");
          if (lim_idx != -1 && tokens[lim_idx].type == JSMN_PRIMITIVE) {
            vp->limit = get_tok_double(js, &tokens[lim_idx]);
            vp->has_limit = true;
          }
          int fader_idx =
              find_object_key(js, tokens, count, params_idx, "fader");
          if (fader_idx != -1) {
            if (tokens[fader_idx].type == JSMN_STRING) {
              char f_str[64];
              get_tok_string(js, &tokens[fader_idx], f_str, sizeof(f_str));
              vp->fader = fader_from_string(f_str);
            } else if (tokens[fader_idx].type == JSMN_PRIMITIVE) {
              vp->fader = (fader_t)get_tok_int(js, &tokens[fader_idx]);
            }
          } else {
            vp->fader = FADER_MAIN;
          }
          break;
        }
        case FILTER_TYPE_LOUDNESS: {
          loudness_parameters_t* lp = &f_conf->parameters.loudness;
          int ref_idx =
              find_object_key(js, tokens, count, params_idx, "reference_level");
          if (ref_idx != -1 && tokens[ref_idx].type == JSMN_PRIMITIVE) {
            lp->reference_level = get_tok_double(js, &tokens[ref_idx]);
            lp->has_reference_level = true;
          }
          int hb_idx =
              find_object_key(js, tokens, count, params_idx, "high_boost");
          if (hb_idx != -1 && tokens[hb_idx].type == JSMN_PRIMITIVE) {
            lp->high_boost = get_tok_double(js, &tokens[hb_idx]);
            lp->has_high_boost = true;
          }
          int lb_idx =
              find_object_key(js, tokens, count, params_idx, "low_boost");
          if (lb_idx != -1 && tokens[lb_idx].type == JSMN_PRIMITIVE) {
            lp->low_boost = get_tok_double(js, &tokens[lb_idx]);
            lp->has_low_boost = true;
          }
          int att_idx =
              find_object_key(js, tokens, count, params_idx, "attenuate_mid");
          if (att_idx != -1 && tokens[att_idx].type == JSMN_PRIMITIVE) {
            lp->attenuate_mid = get_tok_bool(js, &tokens[att_idx]);
          }
          int fader_idx =
              find_object_key(js, tokens, count, params_idx, "fader");
          if (fader_idx != -1) {
            if (tokens[fader_idx].type == JSMN_STRING) {
              char f_str[64];
              get_tok_string(js, &tokens[fader_idx], f_str, sizeof(f_str));
              lp->fader = fader_from_string(f_str);
            } else if (tokens[fader_idx].type == JSMN_PRIMITIVE) {
              lp->fader = (fader_t)get_tok_int(js, &tokens[fader_idx]);
            }
          } else {
            lp->fader = FADER_MAIN;
          }
          break;
        }
        case FILTER_TYPE_BIQUAD: {
          biquad_parameters_t* bp = &f_conf->parameters.biquad;
          int btype_idx =
              find_object_key(js, tokens, count, filter_val_idx, "type");
          if (btype_idx != -1 && tokens[btype_idx].type == JSMN_STRING) {
            char bt_str[64];
            get_tok_string(js, &tokens[btype_idx], bt_str, sizeof(bt_str));
            if (strcmp(bt_str, "Free") == 0)
              bp->type = BIQUAD_TYPE_FREE;
            else if (strcmp(bt_str, "Highpass") == 0)
              bp->type = BIQUAD_TYPE_HIGHPASS;
            else if (strcmp(bt_str, "Lowpass") == 0)
              bp->type = BIQUAD_TYPE_LOWPASS;
            else if (strcmp(bt_str, "HighpassFO") == 0)
              bp->type = BIQUAD_TYPE_HIGHPASS_FO;
            else if (strcmp(bt_str, "LowpassFO") == 0)
              bp->type = BIQUAD_TYPE_LOWPASS_FO;
            else if (strcmp(bt_str, "Highshelf") == 0)
              bp->type = BIQUAD_TYPE_HIGHSHELF;
            else if (strcmp(bt_str, "Lowshelf") == 0)
              bp->type = BIQUAD_TYPE_LOWSHELF;
            else if (strcmp(bt_str, "HighshelfFO") == 0)
              bp->type = BIQUAD_TYPE_HIGHSHELF_FO;
            else if (strcmp(bt_str, "LowshelfFO") == 0)
              bp->type = BIQUAD_TYPE_LOWSHELF_FO;
            else if (strcmp(bt_str, "Peaking") == 0)
              bp->type = BIQUAD_TYPE_PEAKING;
            else if (strcmp(bt_str, "Notch") == 0)
              bp->type = BIQUAD_TYPE_NOTCH;
            else if (strcmp(bt_str, "Bandpass") == 0)
              bp->type = BIQUAD_TYPE_BANDPASS;
            else if (strcmp(bt_str, "Allpass") == 0)
              bp->type = BIQUAD_TYPE_ALLPASS;
            else if (strcmp(bt_str, "AllpassFO") == 0)
              bp->type = BIQUAD_TYPE_ALLPASS_FO;
            else if (strcmp(bt_str, "GeneralNotch") == 0)
              bp->type = BIQUAD_TYPE_GENERAL_NOTCH;
            else if (strcmp(bt_str, "LinkwitzTransform") == 0)
              bp->type = BIQUAD_TYPE_LINKWITZ_TRANSFORM;
          }
          int freq_idx = find_object_key(js, tokens, count, params_idx, "freq");
          if (freq_idx != -1 && tokens[freq_idx].type == JSMN_PRIMITIVE)
            bp->freq = get_tok_double(js, &tokens[freq_idx]);
          int g_idx = find_object_key(js, tokens, count, params_idx, "gain");
          if (g_idx != -1 && tokens[g_idx].type == JSMN_PRIMITIVE)
            bp->gain = get_tok_double(js, &tokens[g_idx]);
          int q_idx = find_object_key(js, tokens, count, params_idx, "q");
          if (q_idx != -1 && tokens[q_idx].type == JSMN_PRIMITIVE)
            bp->q = get_tok_double(js, &tokens[q_idx]);
          int bw_idx =
              find_object_key(js, tokens, count, params_idx, "bandwidth");
          if (bw_idx != -1 && tokens[bw_idx].type == JSMN_PRIMITIVE)
            bp->bandwidth = get_tok_double(js, &tokens[bw_idx]);
          int sl_idx = find_object_key(js, tokens, count, params_idx, "slope");
          if (sl_idx != -1 && tokens[sl_idx].type == JSMN_PRIMITIVE)
            bp->slope = get_tok_double(js, &tokens[sl_idx]);
          int a1_idx = find_object_key(js, tokens, count, params_idx, "a1");
          if (a1_idx != -1 && tokens[a1_idx].type == JSMN_PRIMITIVE)
            bp->a1 = get_tok_double(js, &tokens[a1_idx]);
          int a2_idx = find_object_key(js, tokens, count, params_idx, "a2");
          if (a2_idx != -1 && tokens[a2_idx].type == JSMN_PRIMITIVE)
            bp->a2 = get_tok_double(js, &tokens[a2_idx]);
          int b0_idx = find_object_key(js, tokens, count, params_idx, "b0");
          if (b0_idx != -1 && tokens[b0_idx].type == JSMN_PRIMITIVE)
            bp->b0 = get_tok_double(js, &tokens[b0_idx]);
          int b1_idx = find_object_key(js, tokens, count, params_idx, "b1");
          if (b1_idx != -1 && tokens[b1_idx].type == JSMN_PRIMITIVE)
            bp->b1 = get_tok_double(js, &tokens[b1_idx]);
          int b2_idx = find_object_key(js, tokens, count, params_idx, "b2");
          if (b2_idx != -1 && tokens[b2_idx].type == JSMN_PRIMITIVE)
            bp->b2 = get_tok_double(js, &tokens[b2_idx]);
          int fn_idx =
              find_object_key(js, tokens, count, params_idx, "freq_notch");
          if (fn_idx != -1 && tokens[fn_idx].type == JSMN_PRIMITIVE)
            bp->freq_notch = get_tok_double(js, &tokens[fn_idx]);
          int fp_idx =
              find_object_key(js, tokens, count, params_idx, "freq_pole");
          if (fp_idx != -1 && tokens[fp_idx].type == JSMN_PRIMITIVE)
            bp->freq_pole = get_tok_double(js, &tokens[fp_idx]);
          int qp_idx = find_object_key(js, tokens, count, params_idx, "q_p");
          if (qp_idx != -1 && tokens[qp_idx].type == JSMN_PRIMITIVE)
            bp->q_p = get_tok_double(js, &tokens[qp_idx]);
          int norm_idx =
              find_object_key(js, tokens, count, params_idx, "normalize_at_dc");
          if (norm_idx != -1 && tokens[norm_idx].type == JSMN_PRIMITIVE)
            bp->normalize_at_dc = get_tok_bool(js, &tokens[norm_idx]);
          int fa_idx =
              find_object_key(js, tokens, count, params_idx, "freq_act");
          if (fa_idx != -1 && tokens[fa_idx].type == JSMN_PRIMITIVE)
            bp->freq_act = get_tok_double(js, &tokens[fa_idx]);
          int qa_idx = find_object_key(js, tokens, count, params_idx, "q_act");
          if (qa_idx != -1 && tokens[qa_idx].type == JSMN_PRIMITIVE)
            bp->q_act = get_tok_double(js, &tokens[qa_idx]);
          int ft_idx =
              find_object_key(js, tokens, count, params_idx, "freq_target");
          if (ft_idx != -1 && tokens[ft_idx].type == JSMN_PRIMITIVE)
            bp->freq_target = get_tok_double(js, &tokens[ft_idx]);
          int qt_idx =
              find_object_key(js, tokens, count, params_idx, "q_target");
          if (qt_idx != -1 && tokens[qt_idx].type == JSMN_PRIMITIVE)
            bp->q_target = get_tok_double(js, &tokens[qt_idx]);
          break;
        }
        case FILTER_TYPE_DELAY: {
          delay_parameters_t* dp = &f_conf->parameters.delay;
          int del_idx = find_object_key(js, tokens, count, params_idx, "delay");
          if (del_idx != -1 && tokens[del_idx].type == JSMN_PRIMITIVE) {
            dp->delay = get_tok_double(js, &tokens[del_idx]);
          }
          int unit_idx = find_object_key(js, tokens, count, params_idx, "unit");
          if (unit_idx != -1 && tokens[unit_idx].type == JSMN_STRING) {
            char u_str[32];
            get_tok_string(js, &tokens[unit_idx], u_str, sizeof(u_str));
            if (strcmp(u_str, "ms") == 0)
              dp->unit = DELAY_UNIT_MS;
            else if (strcmp(u_str, "us") == 0)
              dp->unit = DELAY_UNIT_US;
            else if (strcmp(u_str, "samples") == 0)
              dp->unit = DELAY_UNIT_SAMPLES;
            else if (strcmp(u_str, "mm") == 0)
              dp->unit = DELAY_UNIT_MM;
          } else {
            dp->unit = DELAY_UNIT_MS;
          }
          int sub_idx =
              find_object_key(js, tokens, count, params_idx, "subsample");
          if (sub_idx != -1 && tokens[sub_idx].type == JSMN_PRIMITIVE) {
            dp->subsample = get_tok_bool(js, &tokens[sub_idx]);
          }
          break;
        }
        case FILTER_TYPE_CONV: {
          conv_parameters_t* cp = &f_conf->parameters.conv;
          int ctype_idx =
              find_object_key(js, tokens, count, params_idx, "type");
          if (ctype_idx != -1 && tokens[ctype_idx].type == JSMN_STRING) {
            char ct_str[32];
            get_tok_string(js, &tokens[ctype_idx], ct_str, sizeof(ct_str));
            if (strcmp(ct_str, "Values") == 0)
              cp->type = CONV_TYPE_VALUES;
            else if (strcmp(ct_str, "Wav") == 0)
              cp->type = CONV_TYPE_WAV;
            else if (strcmp(ct_str, "Raw") == 0)
              cp->type = CONV_TYPE_RAW;
            else
              cp->type = CONV_TYPE_DUMMY;
          }
          int val_idx =
              find_object_key(js, tokens, count, params_idx, "values");
          if (val_idx != -1 && tokens[val_idx].type == JSMN_ARRAY) {
            int v_size = tokens[val_idx].size;
            cp->values = (double*)calloc(v_size, sizeof(double));
            cp->values_count = v_size;
            for (int v = 0; v < v_size; v++) {
              int el_idx = get_array_element(tokens, count, val_idx, v);
              if (el_idx != -1 && tokens[el_idx].type == JSMN_PRIMITIVE) {
                cp->values[v] = get_tok_double(js, &tokens[el_idx]);
              }
            }
          }
          int file_idx =
              find_object_key(js, tokens, count, params_idx, "filename");
          if (file_idx != -1 && tokens[file_idx].type == JSMN_STRING) {
            get_tok_string(js, &tokens[file_idx], cp->filename,
                           sizeof(cp->filename));
          }
          int fmt_idx =
              find_object_key(js, tokens, count, params_idx, "format");
          if (fmt_idx != -1 && tokens[fmt_idx].type == JSMN_STRING) {
            get_tok_string(js, &tokens[fmt_idx], cp->format,
                           sizeof(cp->format));
          }
          int chan_idx =
              find_object_key(js, tokens, count, params_idx, "channel");
          if (chan_idx != -1 && tokens[chan_idx].type == JSMN_PRIMITIVE) {
            cp->channel = get_tok_int(js, &tokens[chan_idx]);
          }
          break;
        }
        case FILTER_TYPE_BIQUAD_COMBO: {
          biquad_combo_parameters_t* bcp = &f_conf->parameters.biquad_combo;
          int bctype_idx =
              find_object_key(js, tokens, count, params_idx, "type");
          if (bctype_idx != -1 && tokens[bctype_idx].type == JSMN_STRING) {
            char bct_str[64];
            get_tok_string(js, &tokens[bctype_idx], bct_str, sizeof(bct_str));
            if (strcmp(bct_str, "ButterworthHighpass") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS;
            else if (strcmp(bct_str, "ButterworthLowpass") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS;
            else if (strcmp(bct_str, "LinkwitzRileyHighpass") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS;
            else if (strcmp(bct_str, "LinkwitzRileyLowpass") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS;
            else if (strcmp(bct_str, "Tilt") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_TILT;
            else if (strcmp(bct_str, "FivePointPEQ") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_FIVE_POINT_PEQ;
            else if (strcmp(bct_str, "GraphicEqualizer") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER;
          }
          int f_idx = find_object_key(js, tokens, count, params_idx, "freq");
          if (f_idx != -1 && tokens[f_idx].type == JSMN_PRIMITIVE) {
            bcp->freq = get_tok_double(js, &tokens[f_idx]);
            bcp->has_freq = true;
          }
          int order_idx =
              find_object_key(js, tokens, count, params_idx, "order");
          if (order_idx != -1 && tokens[order_idx].type == JSMN_PRIMITIVE) {
            bcp->order = get_tok_int(js, &tokens[order_idx]);
            bcp->has_order = true;
          }
          int gain_idx = find_object_key(js, tokens, count, params_idx, "gain");
          if (gain_idx != -1 && tokens[gain_idx].type == JSMN_PRIMITIVE) {
            bcp->gain = get_tok_double(js, &tokens[gain_idx]);
            bcp->has_gain = true;
          }
          int gains_idx =
              find_object_key(js, tokens, count, params_idx, "gains");
          if (gains_idx != -1 && tokens[gains_idx].type == JSMN_ARRAY) {
            int g_size = tokens[gains_idx].size;
            bcp->gains = (double*)calloc(g_size, sizeof(double));
            bcp->gains_count = g_size;
            for (int g = 0; g < g_size; g++) {
              int el_idx = get_array_element(tokens, count, gains_idx, g);
              if (el_idx != -1 && tokens[el_idx].type == JSMN_PRIMITIVE) {
                bcp->gains[g] = get_tok_double(js, &tokens[el_idx]);
              }
            }
          }
          break;
        }
        default:
          break;
      }
    }

    filter_key_idx = skip_token(tokens, filter_val_idx);
  }
  return 0;
}

int dsp_config_parse_json(const char* json, dsp_config_t** out_config,
                          config_error_t* err) {
  if (!json || !out_config) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "JSON string or output pointer is NULL");
    return -1;
  }

  dsp_config_t* config = (dsp_config_t*)calloc(1, sizeof(dsp_config_t));
  if (!config) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return -1;
  }

  jsmn_parser p;
  jsmn_init(&p);

  int num_tokens = jsmn_parse(&p, json, strlen(json), NULL, 0);
  if (num_tokens < 0) {
    free(config);
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to parse JSON tokens (syntax error)");
    return -1;
  }

  jsmntok_t* tokens = (jsmntok_t*)malloc(num_tokens * sizeof(jsmntok_t));
  if (!tokens) {
    free(config);
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return -1;
  }

  jsmn_init(&p);
  int count = jsmn_parse(&p, json, strlen(json), tokens, num_tokens);
  if (count < 0) {
    free(tokens);
    free(config);
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to parse JSON structure");
    return -1;
  }

  int devices_idx = find_top_level_key(json, tokens, count, "devices");
  if (devices_idx == -1) {
    free(tokens);
    free(config);
    config_error_set(err, CONFIG_ERR_PARSE, "Config must contain 'devices'");
    return -1;
  }
  if (parse_devices(json, tokens, count, devices_idx, config, err) != 0) {
    free(tokens);
    dsp_config_free(config);
    return -1;
  }

  int pipeline_idx = find_top_level_key(json, tokens, count, "pipeline");
  if (pipeline_idx != -1) {
    if (parse_pipeline(json, tokens, count, pipeline_idx, config, err) != 0) {
      free(tokens);
      dsp_config_free(config);
      return -1;
    }
  }

  int mixers_idx = find_top_level_key(json, tokens, count, "mixers");
  if (mixers_idx != -1) {
    if (parse_mixers(json, tokens, count, mixers_idx, config, err) != 0) {
      free(tokens);
      dsp_config_free(config);
      return -1;
    }
  }

  int filters_idx = find_top_level_key(json, tokens, count, "filters");
  if (filters_idx != -1) {
    if (parse_filters(json, tokens, count, filters_idx, config, err) != 0) {
      free(tokens);
      dsp_config_free(config);
      return -1;
    }
  }

  free(tokens);

  if (dsp_config_validate(config, err) != 0) {
    dsp_config_free(config);
    return -1;
  }

  *out_config = config;
  return 0;
}
