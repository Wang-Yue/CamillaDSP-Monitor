#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "configuration.h"

/**
 * @brief Parses an array of string labels from a cJSON array.
 *
 * Allocates a string array and duplicates each label string.
 *
 * @param labels_arr The cJSON array containing the labels.
 * @param out_labels Output pointer to store the allocated array of string
 * pointers.
 * @param out_count Output pointer to store the size of the parsed labels array.
 * @param out_has_labels Output pointer to set to true if labels were
 * successfully parsed.
 */
static void parse_labels_array(const cJSON* labels_arr, char*** out_labels,
                               size_t* out_count, bool* out_has_labels) {
  if (!cJSON_IsArray(labels_arr)) return;
  int size = cJSON_GetArraySize(labels_arr);
  if (size <= 0) return;

  char** arr = (char**)calloc(size, sizeof(char*));
  if (!arr) return;

  for (int k = 0; k < size; k++) {
    cJSON* el = cJSON_GetArrayItem(labels_arr, k);
    if (cJSON_IsString(el) && el->valuestring) {
      arr[k] = strdup(el->valuestring);
    } else if (cJSON_IsNull(el)) {
      arr[k] = NULL;
    }
  }

  *out_labels = arr;
  *out_count = (size_t)size;
  *out_has_labels = true;
}

static double* parse_double_array(const cJSON* arr, size_t* out_count) {
  if (!cJSON_IsArray(arr)) {
    *out_count = 0;
    return NULL;
  }
  int size = cJSON_GetArraySize(arr);
  if (size <= 0) {
    *out_count = 0;
    return NULL;
  }
  double* values = (double*)calloc(size, sizeof(double));
  if (!values) {
    *out_count = 0;
    return NULL;
  }
  for (int i = 0; i < size; i++) {
    cJSON* el = cJSON_GetArrayItem(arr, i);
    if (cJSON_IsNumber(el)) {
      values[i] = el->valuedouble;
    }
  }
  *out_count = (size_t)size;
  return values;
}

static int* parse_int_array(const cJSON* arr, size_t* out_count) {
  if (!cJSON_IsArray(arr)) {
    *out_count = 0;
    return NULL;
  }
  int size = cJSON_GetArraySize(arr);
  if (size <= 0) {
    *out_count = 0;
    return NULL;
  }
  int* values = (int*)calloc(size, sizeof(int));
  if (!values) {
    *out_count = 0;
    return NULL;
  }
  for (int i = 0; i < size; i++) {
    cJSON* el = cJSON_GetArrayItem(arr, i);
    if (cJSON_IsNumber(el)) {
      values[i] = el->valueint;
    }
  }
  *out_count = (size_t)size;
  return values;
}

/**
 * @brief Parses the resampler configuration section.
 *
 * Parses the "resampler" JSON object and populates the devices configuration.
 *
 * @param res_obj The cJSON object containing resampler settings.
 * @param devices Pointer to the devices configuration structure to populate.
 */
static void parse_resampler(const cJSON* res_obj, devices_config_t* devices) {
  if (!cJSON_IsObject(res_obj)) return;
  resampler_config_t* res = &devices->resampler;
  devices->has_resampler = true;

  cJSON* item;

  item = cJSON_GetObjectItemCaseSensitive(res_obj, "type");
  if (cJSON_IsString(item) && item->valuestring) {
    res->type = resampler_type_from_string(item->valuestring);
  }

  item = cJSON_GetObjectItemCaseSensitive(res_obj, "profile");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(res->profile, item->valuestring, sizeof(res->profile) - 1);
    res->has_profile = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(res_obj, "interpolation");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(res->interpolation, item->valuestring,
            sizeof(res->interpolation) - 1);
    res->has_interpolation = true;
  }

#if defined(ENABLE_COREAUDIO)
  item = cJSON_GetObjectItemCaseSensitive(res_obj, "apple_quality");
  if (cJSON_IsString(item) && item->valuestring) {
    res->apple_quality = apple_resampler_quality_from_string(item->valuestring);
    res->has_apple_quality = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(res_obj, "apple_complexity");
  if (cJSON_IsString(item) && item->valuestring) {
    res->apple_complexity =
        apple_resampler_complexity_from_string(item->valuestring);
    res->has_apple_complexity = true;
  }
#endif

  item = cJSON_GetObjectItemCaseSensitive(res_obj, "sinc_len");
  if (cJSON_IsNumber(item)) {
    res->sinc_len = item->valueint;
    res->has_sinc_len = (res->sinc_len > 0);
  }

  item = cJSON_GetObjectItemCaseSensitive(res_obj, "oversampling_factor");
  if (cJSON_IsNumber(item)) {
    res->oversampling_factor = item->valueint;
    res->has_oversampling_factor = (res->oversampling_factor > 0);
  }

  item = cJSON_GetObjectItemCaseSensitive(res_obj, "window");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(res->window, item->valuestring, sizeof(res->window) - 1);
    res->has_window = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(res_obj, "f_cutoff");
  if (cJSON_IsNumber(item)) {
    res->f_cutoff = item->valuedouble;
    res->has_f_cutoff = (res->f_cutoff > 0.0);
  }
}

typedef struct {
  audio_backend_type_t type;
  int channels;
  char device[256];
  bool has_device;
#if defined(ENABLE_COREAUDIO)
  coreaudio_sample_format_t format;
  bool has_format;
#endif
#if defined(ENABLE_ALSA)
  alsa_sample_format_t alsa_format;
  bool has_alsa_format;
#endif
  bool bypass_dop;
  bool has_bypass_dop;
  double dop_cutoff_hz;
  bool has_dop_cutoff_hz;
  generator_signal_t generator;

  char filename[512];
  bool has_filename;
  binary_sample_format_t file_format;
  bool has_file_format;
  bool is_wav;
  bool has_is_wav;
  int skip_bytes;
  bool has_skip_bytes;
  int read_bytes;
  bool has_read_bytes;
  int extra_samples;
  bool has_extra_samples;
  char** labels;
  size_t labels_count;
  bool has_labels;

  // Win32 / WASAPI / ASIO Capture fields
  bool loopback;
  bool has_loopback;
  bool exclusive;
  bool has_exclusive;
  bool polling;
  bool has_polling;
#if defined(ENABLE_WASAPI)
  wasapi_sample_format_t wasapi_format;
  bool has_wasapi_format;
#endif
#if defined(ENABLE_ASIO)
  asio_sample_format_t asio_format;
  bool has_asio_format;
#endif

  // ALSA / Pulse / PipeWire Capture fields
  bool stop_on_inactive;
  bool has_stop_on_inactive;
  char link_volume_control[256];
  bool has_link_volume_control;
  char link_mute_control[256];
  bool has_link_mute_control;

  // PipeWire Capture fields
  char node_name[256];
  bool has_node_name;
  char node_description[256];
  bool has_node_description;
  char node_group_name[256];
  bool has_node_group_name;
  char autoconnect_to[256];
  bool has_autoconnect_to;

  // Bluez Capture fields
  char service[256];
  bool has_service;
  char dbus_path[256];
  bool has_dbus_path;
#if defined(ENABLE_BLUEZ)
  binary_sample_format_t bluez_format;
#endif
} flat_capture_device_config_t;

/**
 * @brief Parses the capture device configuration section.
 *
 * Parses the "capture" JSON object and populates the capture configuration.
 * Supports various audio backends such as CoreAudio, ALSA, WASAPI, Bluez,
 * File, and Generator, mapping the values into a union layout.
 *
 * @param cap_obj The cJSON object containing capture settings.
 * @param devices Pointer to the devices configuration structure to populate.
 */
static void parse_capture(const cJSON* cap_obj, devices_config_t* devices) {
  if (!cJSON_IsObject(cap_obj)) return;

  flat_capture_device_config_t temp;
  memset(&temp, 0, sizeof(temp));
  flat_capture_device_config_t* cap = &temp;

  cJSON* item;

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "channels");
  if (cJSON_IsNumber(item)) {
    cap->channels = item->valueint;
  }

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "type");
  char type_str[64] = "";
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(type_str, item->valuestring, sizeof(type_str) - 1);
    cap->type = audio_backend_type_from_string(type_str);
    if (strcasecmp(type_str, "WavFile") == 0) {
      cap->is_wav = true;
      cap->has_is_wav = true;
    }
  } else {
#if defined(ENABLE_COREAUDIO)
    cap->type = AUDIO_BACKEND_TYPE_CORE_AUDIO;
#elif defined(ENABLE_ALSA)
    cap->type = AUDIO_BACKEND_TYPE_ALSA;
#elif defined(ENABLE_WASAPI)
    cap->type = AUDIO_BACKEND_TYPE_WASAPI;
#else
    cap->type = AUDIO_BACKEND_TYPE_FILE;
#endif
  }

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "device");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->device, item->valuestring, sizeof(cap->device) - 1);
    cap->has_device = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "filename");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->filename, item->valuestring, sizeof(cap->filename) - 1);
    cap->has_filename = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "format");
  if (cJSON_IsString(item) && item->valuestring) {
    if (cap->type == AUDIO_BACKEND_TYPE_FILE ||
        cap->type == AUDIO_BACKEND_TYPE_STDIN_OUT) {
      cap->file_format = binary_sample_format_from_string(item->valuestring);
      cap->has_file_format = true;
#if defined(ENABLE_WASAPI)
    } else if (cap->type == AUDIO_BACKEND_TYPE_WASAPI) {
      cap->wasapi_format = wasapi_sample_format_from_string(item->valuestring);
      cap->has_wasapi_format = true;
#endif
#if defined(ENABLE_ASIO)
    } else if (cap->type == AUDIO_BACKEND_TYPE_ASIO) {
      cap->asio_format = asio_sample_format_from_string(item->valuestring);
      cap->has_asio_format = true;
#endif
#if defined(ENABLE_BLUEZ)
    } else if (cap->type == AUDIO_BACKEND_TYPE_BLUEZ) {
      cap->bluez_format = binary_sample_format_from_string(item->valuestring);
#endif
#if defined(ENABLE_ALSA)
    } else if (cap->type == AUDIO_BACKEND_TYPE_ALSA) {
      cap->alsa_format = alsa_sample_format_from_string(item->valuestring);
      cap->has_alsa_format = true;
#endif
#if defined(ENABLE_COREAUDIO)
    } else if (cap->type == AUDIO_BACKEND_TYPE_CORE_AUDIO) {
      cap->format = coreaudio_sample_format_from_string(item->valuestring);
      cap->has_format = true;
#endif
    }
  }

#if defined(_WIN32)
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "loopback");
  if (cJSON_IsBool(item)) {
    cap->loopback = cJSON_IsTrue(item);
    cap->has_loopback = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "exclusive");
  if (cJSON_IsBool(item)) {
    cap->exclusive = cJSON_IsTrue(item);
    cap->has_exclusive = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "polling");
  if (cJSON_IsBool(item)) {
    cap->polling = cJSON_IsTrue(item);
    cap->has_polling = true;
  }
#endif

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "skip_bytes");
  if (cJSON_IsNumber(item)) {
    cap->skip_bytes = item->valueint;
    cap->has_skip_bytes = (cap->skip_bytes > 0);
  }

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "read_bytes");
  if (cJSON_IsNumber(item)) {
    cap->read_bytes = item->valueint;
    cap->has_read_bytes = (cap->read_bytes > 0);
  }

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "extra_samples");
  if (cJSON_IsNumber(item)) {
    cap->extra_samples = item->valueint;
    cap->has_extra_samples = (cap->extra_samples > 0);
  }

#if defined(ENABLE_ALSA) || defined(ENABLE_PULSE) || defined(ENABLE_PIPEWIRE)
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "stop_on_inactive");
  if (cJSON_IsBool(item)) {
    cap->stop_on_inactive = cJSON_IsTrue(item);
    cap->has_stop_on_inactive = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "link_volume_control");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->link_volume_control, item->valuestring,
            sizeof(cap->link_volume_control) - 1);
    cap->has_link_volume_control = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "link_mute_control");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->link_mute_control, item->valuestring,
            sizeof(cap->link_mute_control) - 1);
    cap->has_link_mute_control = true;
  }
#endif

#if defined(ENABLE_PIPEWIRE)
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "node_name");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->node_name, item->valuestring, sizeof(cap->node_name) - 1);
    cap->has_node_name = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "node_description");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->node_description, item->valuestring,
            sizeof(cap->node_description) - 1);
    cap->has_node_description = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "node_group_name");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->node_group_name, item->valuestring,
            sizeof(cap->node_group_name) - 1);
    cap->has_node_group_name = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "autoconnect_to");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->autoconnect_to, item->valuestring,
            sizeof(cap->autoconnect_to) - 1);
    cap->has_autoconnect_to = true;
  }
#endif

  parse_labels_array(cJSON_GetObjectItemCaseSensitive(cap_obj, "labels"),
                     &cap->labels, &cap->labels_count, &cap->has_labels);

#if defined(ENABLE_BLUEZ)
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "service");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->service, item->valuestring, sizeof(cap->service) - 1);
    cap->has_service = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "dbus_path");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->dbus_path, item->valuestring, sizeof(cap->dbus_path) - 1);
    cap->has_dbus_path = true;
  }
#endif

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "bypass_dop");
  if (cJSON_IsBool(item)) {
    cap->bypass_dop = cJSON_IsTrue(item);
    cap->has_bypass_dop = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "dop_cutoff_hz");
  if (cJSON_IsNumber(item)) {
    cap->dop_cutoff_hz = item->valuedouble;
    cap->has_dop_cutoff_hz = true;
  }

  cJSON* sig_obj = cJSON_GetObjectItemCaseSensitive(cap_obj, "signal");
  if (cJSON_IsObject(sig_obj)) {
    cJSON* sig_type = cJSON_GetObjectItemCaseSensitive(sig_obj, "type");
    if (cJSON_IsString(sig_type) && sig_type->valuestring) {
      cap->generator.type = signal_type_from_string(sig_type->valuestring);
    } else {
      cap->generator.type = SIGNAL_TYPE_SINE;
    }
    cJSON* freq = cJSON_GetObjectItemCaseSensitive(sig_obj, "freq");
    if (cJSON_IsNumber(freq)) {
      cap->generator.frequency = freq->valuedouble;
    } else {
      cap->generator.frequency = 1000.0;
    }
    cJSON* level = cJSON_GetObjectItemCaseSensitive(sig_obj, "level");
    if (cJSON_IsNumber(level)) {
      cap->generator.level = level->valuedouble;
    } else {
      cap->generator.level = 0.0;
    }
  }

  // Copy flat temp to union configuration
  capture_device_config_t* final_cap = &devices->capture;
  final_cap->type = temp.type;
  final_cap->labels = temp.labels;
  final_cap->labels_count = temp.labels_count;
  final_cap->has_labels = temp.has_labels;
  final_cap->is_wav = temp.is_wav;
  final_cap->has_is_wav = temp.has_is_wav;
  final_cap->bypass_dop = temp.has_bypass_dop ? temp.bypass_dop : true;
  final_cap->has_bypass_dop = temp.has_bypass_dop;
  final_cap->dop_cutoff_hz =
      temp.has_dop_cutoff_hz ? temp.dop_cutoff_hz : 20000.0;
  final_cap->has_dop_cutoff_hz = temp.has_dop_cutoff_hz;

  switch (temp.type) {
#if defined(ENABLE_COREAUDIO)
    case AUDIO_BACKEND_TYPE_CORE_AUDIO:
      final_cap->cfg.coreaudio.channels = temp.channels;
      snprintf(final_cap->cfg.coreaudio.device,
               sizeof(final_cap->cfg.coreaudio.device), "%s", temp.device);
      final_cap->cfg.coreaudio.has_device = temp.has_device;
      final_cap->cfg.coreaudio.format = temp.format;
      final_cap->cfg.coreaudio.has_format = temp.has_format;
      break;
#endif
#if defined(ENABLE_ALSA)
    case AUDIO_BACKEND_TYPE_ALSA:
      final_cap->cfg.alsa.channels = temp.channels;
      snprintf(final_cap->cfg.alsa.device, sizeof(final_cap->cfg.alsa.device),
               "%s", temp.device);
      final_cap->cfg.alsa.format = temp.alsa_format;
      final_cap->cfg.alsa.has_format = temp.has_alsa_format;
      final_cap->cfg.alsa.stop_on_inactive = temp.stop_on_inactive;
      final_cap->cfg.alsa.has_stop_on_inactive = temp.has_stop_on_inactive;
      snprintf(final_cap->cfg.alsa.link_volume_control,
               sizeof(final_cap->cfg.alsa.link_volume_control), "%s",
               temp.link_volume_control);
      final_cap->cfg.alsa.has_link_volume_control =
          temp.has_link_volume_control;
      snprintf(final_cap->cfg.alsa.link_mute_control,
               sizeof(final_cap->cfg.alsa.link_mute_control), "%s",
               temp.link_mute_control);
      final_cap->cfg.alsa.has_link_mute_control = temp.has_link_mute_control;
      break;
#endif
#if defined(ENABLE_PULSE)
    case AUDIO_BACKEND_TYPE_PULSE_AUDIO:
      final_cap->cfg.pulse.channels = temp.channels;
      snprintf(final_cap->cfg.pulse.device, sizeof(final_cap->cfg.pulse.device),
               "%s", temp.device);
      break;
#endif
#if defined(ENABLE_PIPEWIRE)
    case AUDIO_BACKEND_TYPE_PIPEWIRE:
      final_cap->cfg.pipewire.channels = temp.channels;
      snprintf(final_cap->cfg.pipewire.device,
               sizeof(final_cap->cfg.pipewire.device), "%s", temp.device);
      final_cap->cfg.pipewire.has_device = temp.has_device;
      snprintf(final_cap->cfg.pipewire.node_name,
               sizeof(final_cap->cfg.pipewire.node_name), "%s", temp.node_name);
      final_cap->cfg.pipewire.has_node_name = temp.has_node_name;
      snprintf(final_cap->cfg.pipewire.node_description,
               sizeof(final_cap->cfg.pipewire.node_description), "%s",
               temp.node_description);
      final_cap->cfg.pipewire.has_node_description = temp.has_node_description;
      snprintf(final_cap->cfg.pipewire.node_group_name,
               sizeof(final_cap->cfg.pipewire.node_group_name), "%s",
               temp.node_group_name);
      final_cap->cfg.pipewire.has_node_group_name = temp.has_node_group_name;
      snprintf(final_cap->cfg.pipewire.autoconnect_to,
               sizeof(final_cap->cfg.pipewire.autoconnect_to), "%s",
               temp.autoconnect_to);
      final_cap->cfg.pipewire.has_autoconnect_to = temp.has_autoconnect_to;
      break;
#endif
#if defined(ENABLE_JACK)
    case AUDIO_BACKEND_TYPE_JACK:
      final_cap->cfg.jack.channels = temp.channels;
      snprintf(final_cap->cfg.jack.device, sizeof(final_cap->cfg.jack.device),
               "%s", temp.device);
      break;
#endif
    case AUDIO_BACKEND_TYPE_FILE:
      if (temp.is_wav) {
        snprintf(final_cap->cfg.wav_file.filename,
                 sizeof(final_cap->cfg.wav_file.filename), "%s", temp.filename);
        final_cap->cfg.wav_file.has_filename = temp.has_filename;
        final_cap->cfg.wav_file.extra_samples = temp.extra_samples;
        final_cap->cfg.wav_file.has_extra_samples = temp.has_extra_samples;
      } else {
        snprintf(final_cap->cfg.raw_file.filename,
                 sizeof(final_cap->cfg.raw_file.filename), "%s", temp.filename);
        final_cap->cfg.raw_file.has_filename = temp.has_filename;
        final_cap->cfg.raw_file.format = temp.file_format;
        final_cap->cfg.raw_file.has_format = temp.has_file_format;
        final_cap->cfg.raw_file.channels = temp.channels;
        final_cap->cfg.raw_file.skip_bytes = temp.skip_bytes;
        final_cap->cfg.raw_file.has_skip_bytes = temp.has_skip_bytes;
        final_cap->cfg.raw_file.read_bytes = temp.read_bytes;
        final_cap->cfg.raw_file.has_read_bytes = temp.has_read_bytes;
        final_cap->cfg.raw_file.extra_samples = temp.extra_samples;
        final_cap->cfg.raw_file.has_extra_samples = temp.has_extra_samples;
      }
      break;
    case AUDIO_BACKEND_TYPE_STDIN_OUT:
      final_cap->cfg.stdin_in.channels = temp.channels;
      final_cap->cfg.stdin_in.format = temp.file_format;
      final_cap->cfg.stdin_in.extra_samples = temp.extra_samples;
      final_cap->cfg.stdin_in.has_extra_samples = temp.has_extra_samples;
      final_cap->cfg.stdin_in.skip_bytes = temp.skip_bytes;
      final_cap->cfg.stdin_in.has_skip_bytes = temp.has_skip_bytes;
      final_cap->cfg.stdin_in.read_bytes = temp.read_bytes;
      final_cap->cfg.stdin_in.has_read_bytes = temp.has_read_bytes;
      break;
    case AUDIO_BACKEND_TYPE_GENERATOR:
      final_cap->cfg.generator.channels = temp.channels;
      final_cap->cfg.generator.signal = temp.generator;
      break;
#if defined(ENABLE_WASAPI)
    case AUDIO_BACKEND_TYPE_WASAPI:
      final_cap->cfg.wasapi.channels = temp.channels;
      snprintf(final_cap->cfg.wasapi.device,
               sizeof(final_cap->cfg.wasapi.device), "%s", temp.device);
      final_cap->cfg.wasapi.has_device = temp.has_device;
      final_cap->cfg.wasapi.format = temp.wasapi_format;
      final_cap->cfg.wasapi.has_format = temp.has_wasapi_format;
      final_cap->cfg.wasapi.exclusive = temp.exclusive;
      final_cap->cfg.wasapi.has_exclusive = temp.has_exclusive;
      final_cap->cfg.wasapi.loopback = temp.loopback;
      final_cap->cfg.wasapi.has_loopback = temp.has_loopback;
      final_cap->cfg.wasapi.polling = temp.polling;
      final_cap->cfg.wasapi.has_polling = temp.has_polling;
      break;
#endif
#if defined(ENABLE_ASIO)
    case AUDIO_BACKEND_TYPE_ASIO:
      final_cap->cfg.asio.channels = temp.channels;
      snprintf(final_cap->cfg.asio.device, sizeof(final_cap->cfg.asio.device),
               "%s", temp.device);
      final_cap->cfg.asio.format = temp.asio_format;
      final_cap->cfg.asio.has_format = temp.has_asio_format;
      break;
#endif
#if defined(ENABLE_BLUEZ)
    case AUDIO_BACKEND_TYPE_BLUEZ:
      snprintf(final_cap->cfg.bluez.service,
               sizeof(final_cap->cfg.bluez.service), "%s", temp.service);
      final_cap->cfg.bluez.has_service = temp.has_service;
      snprintf(final_cap->cfg.bluez.dbus_path,
               sizeof(final_cap->cfg.bluez.dbus_path), "%s", temp.dbus_path);
      final_cap->cfg.bluez.has_dbus_path = temp.has_dbus_path;
      final_cap->cfg.bluez.format = temp.bluez_format;
      final_cap->cfg.bluez.channels = temp.channels;
      break;
#endif
    default:
      break;
  }
}

typedef struct {
  audio_backend_type_t type;
  int channels;
  char device[256];
  bool has_device;
#if defined(ENABLE_COREAUDIO)
  coreaudio_sample_format_t format;
  bool has_format;
#endif
#if defined(ENABLE_ALSA)
  alsa_sample_format_t alsa_format;
  bool has_alsa_format;
#endif
  bool exclusive;
  bool has_exclusive;
  bool output_dop;
  bool has_output_dop;
  sdm_filter_t dop_encoder_filter;
  bool has_dop_encoder_filter;
  char filename[512];
  bool has_filename;
  binary_sample_format_t file_format;
  bool has_file_format;
  bool is_wav;
  bool has_is_wav;
  char** labels;
  size_t labels_count;
  bool has_labels;

  // Win32 / WASAPI / ASIO fields
  bool polling;
  bool has_polling;
#if defined(ENABLE_WASAPI)
  wasapi_sample_format_t wasapi_format;
  bool has_wasapi_format;
#endif
#if defined(ENABLE_ASIO)
  asio_sample_format_t asio_format;
  bool has_asio_format;
#endif

  // PipeWire fields
  char node_name[256];
  bool has_node_name;
  char node_description[256];
  bool has_node_description;
  char node_group_name[256];
  bool has_node_group_name;
  char autoconnect_to[256];
  bool has_autoconnect_to;
} flat_playback_device_config_t;

/**
 * @brief Parses the playback device configuration section.
 *
 * Parses the "playback" JSON object and populates the playback configuration.
 * Supports various audio backends similar to parse_capture.
 *
 * @param play_obj The cJSON object containing playback settings.
 * @param devices Pointer to the devices configuration structure to populate.
 */
static void parse_playback(const cJSON* play_obj, devices_config_t* devices) {
  if (!cJSON_IsObject(play_obj)) return;

  flat_playback_device_config_t temp;
  memset(&temp, 0, sizeof(temp));
  flat_playback_device_config_t* play = &temp;

  cJSON* item;

  item = cJSON_GetObjectItemCaseSensitive(play_obj, "channels");
  if (cJSON_IsNumber(item)) {
    play->channels = item->valueint;
  }

  item = cJSON_GetObjectItemCaseSensitive(play_obj, "type");
  if (cJSON_IsString(item) && item->valuestring) {
    play->type = audio_backend_type_from_string(item->valuestring);
  } else {
#if defined(ENABLE_COREAUDIO)
    play->type = AUDIO_BACKEND_TYPE_CORE_AUDIO;
#elif defined(ENABLE_ALSA)
    play->type = AUDIO_BACKEND_TYPE_ALSA;
#elif defined(ENABLE_WASAPI)
    play->type = AUDIO_BACKEND_TYPE_WASAPI;
#else
    play->type = AUDIO_BACKEND_TYPE_FILE;
#endif
  }

  item = cJSON_GetObjectItemCaseSensitive(play_obj, "device");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(play->device, item->valuestring, sizeof(play->device) - 1);
    play->has_device = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(play_obj, "filename");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(play->filename, item->valuestring, sizeof(play->filename) - 1);
    play->has_filename = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(play_obj, "format");
  if (cJSON_IsString(item) && item->valuestring) {
    if (play->type == AUDIO_BACKEND_TYPE_FILE ||
        play->type == AUDIO_BACKEND_TYPE_STDIN_OUT) {
      play->file_format = binary_sample_format_from_string(item->valuestring);
      play->has_file_format = true;
#if defined(ENABLE_WASAPI)
    } else if (play->type == AUDIO_BACKEND_TYPE_WASAPI) {
      play->wasapi_format = wasapi_sample_format_from_string(item->valuestring);
      play->has_wasapi_format = true;
#endif
#if defined(ENABLE_ASIO)
    } else if (play->type == AUDIO_BACKEND_TYPE_ASIO) {
      play->asio_format = asio_sample_format_from_string(item->valuestring);
      play->has_asio_format = true;
#endif
#if defined(ENABLE_ALSA)
    } else if (play->type == AUDIO_BACKEND_TYPE_ALSA) {
      play->alsa_format = alsa_sample_format_from_string(item->valuestring);
      play->has_alsa_format = true;
#endif
#if defined(ENABLE_COREAUDIO)
    } else if (play->type == AUDIO_BACKEND_TYPE_CORE_AUDIO) {
      play->format = coreaudio_sample_format_from_string(item->valuestring);
      play->has_format = true;
#endif
    }
  }

  item = cJSON_GetObjectItemCaseSensitive(play_obj, "wav_header");
  if (cJSON_IsBool(item)) {
    play->is_wav = cJSON_IsTrue(item);
    play->has_is_wav = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(play_obj, "exclusive");
  if (cJSON_IsBool(item)) {
    play->exclusive = cJSON_IsTrue(item);
    play->has_exclusive = true;
  }

#if defined(_WIN32)
  item = cJSON_GetObjectItemCaseSensitive(play_obj, "polling");
  if (cJSON_IsBool(item)) {
    play->polling = cJSON_IsTrue(item);
    play->has_polling = true;
  }
#endif

  item = cJSON_GetObjectItemCaseSensitive(play_obj, "output_dop");
  if (cJSON_IsBool(item)) {
    play->output_dop = cJSON_IsTrue(item);
    play->has_output_dop = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(play_obj, "dop_encoder_filter");
  if (cJSON_IsString(item) && item->valuestring) {
    play->dop_encoder_filter = sdm_filter_from_string(item->valuestring);
    play->has_dop_encoder_filter =
        (play->dop_encoder_filter != SDM_FILTER_INVALID);
  }

#if defined(ENABLE_PIPEWIRE)
  item = cJSON_GetObjectItemCaseSensitive(play_obj, "node_name");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(play->node_name, item->valuestring, sizeof(play->node_name) - 1);
    play->has_node_name = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(play_obj, "node_description");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(play->node_description, item->valuestring,
            sizeof(play->node_description) - 1);
    play->has_node_description = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(play_obj, "node_group_name");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(play->node_group_name, item->valuestring,
            sizeof(play->node_group_name) - 1);
    play->has_node_group_name = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(play_obj, "autoconnect_to");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(play->autoconnect_to, item->valuestring,
            sizeof(play->autoconnect_to) - 1);
    play->has_autoconnect_to = true;
  }
#endif

  parse_labels_array(cJSON_GetObjectItemCaseSensitive(play_obj, "labels"),
                     &play->labels, &play->labels_count, &play->has_labels);

  // Copy flat temp to union configuration
  playback_device_config_t* final_play = &devices->playback;
  final_play->type = temp.type;
  final_play->labels = temp.labels;
  final_play->labels_count = temp.labels_count;
  final_play->has_labels = temp.has_labels;
  final_play->is_wav = temp.is_wav;
  final_play->has_is_wav = temp.has_is_wav;
  final_play->output_dop = temp.has_output_dop ? temp.output_dop : false;
  final_play->has_output_dop = temp.has_output_dop;
  final_play->dop_encoder_filter = temp.has_dop_encoder_filter
                                       ? temp.dop_encoder_filter
                                       : SDM_FILTER_INVALID;
  final_play->has_dop_encoder_filter = temp.has_dop_encoder_filter;

  switch (temp.type) {
#if defined(ENABLE_COREAUDIO)
    case AUDIO_BACKEND_TYPE_CORE_AUDIO:
      final_play->cfg.coreaudio.channels = temp.channels;
      snprintf(final_play->cfg.coreaudio.device,
               sizeof(final_play->cfg.coreaudio.device), "%s", temp.device);
      final_play->cfg.coreaudio.has_device = temp.has_device;
      final_play->cfg.coreaudio.format = temp.format;
      final_play->cfg.coreaudio.has_format = temp.has_format;
      final_play->cfg.coreaudio.exclusive = temp.exclusive;
      final_play->cfg.coreaudio.has_exclusive = temp.has_exclusive;
      break;
#endif
#if defined(ENABLE_ALSA)
    case AUDIO_BACKEND_TYPE_ALSA:
      final_play->cfg.alsa.channels = temp.channels;
      snprintf(final_play->cfg.alsa.device, sizeof(final_play->cfg.alsa.device),
               "%s", temp.device);
      final_play->cfg.alsa.format = temp.alsa_format;
      final_play->cfg.alsa.has_format = temp.has_alsa_format;
      break;
#endif
#if defined(ENABLE_PULSE)
    case AUDIO_BACKEND_TYPE_PULSE_AUDIO:
      final_play->cfg.pulse.channels = temp.channels;
      snprintf(final_play->cfg.pulse.device,
               sizeof(final_play->cfg.pulse.device), "%s", temp.device);
      break;
#endif
#if defined(ENABLE_PIPEWIRE)
    case AUDIO_BACKEND_TYPE_PIPEWIRE:
      final_play->cfg.pipewire.channels = temp.channels;
      snprintf(final_play->cfg.pipewire.device,
               sizeof(final_play->cfg.pipewire.device), "%s", temp.device);
      final_play->cfg.pipewire.has_device = temp.has_device;
      snprintf(final_play->cfg.pipewire.node_name,
               sizeof(final_play->cfg.pipewire.node_name), "%s",
               temp.node_name);
      final_play->cfg.pipewire.has_node_name = temp.has_node_name;
      snprintf(final_play->cfg.pipewire.node_description,
               sizeof(final_play->cfg.pipewire.node_description), "%s",
               temp.node_description);
      final_play->cfg.pipewire.has_node_description = temp.has_node_description;
      snprintf(final_play->cfg.pipewire.node_group_name,
               sizeof(final_play->cfg.pipewire.node_group_name), "%s",
               temp.node_group_name);
      final_play->cfg.pipewire.has_node_group_name = temp.has_node_group_name;
      snprintf(final_play->cfg.pipewire.autoconnect_to,
               sizeof(final_play->cfg.pipewire.autoconnect_to), "%s",
               temp.autoconnect_to);
      final_play->cfg.pipewire.has_autoconnect_to = temp.has_autoconnect_to;
      break;
#endif
#if defined(ENABLE_JACK)
    case AUDIO_BACKEND_TYPE_JACK:
      final_play->cfg.jack.channels = temp.channels;
      snprintf(final_play->cfg.jack.device, sizeof(final_play->cfg.jack.device),
               "%s", temp.device);
      break;
#endif
    case AUDIO_BACKEND_TYPE_FILE:
      snprintf(final_play->cfg.raw_file.filename,
               sizeof(final_play->cfg.raw_file.filename), "%s", temp.filename);
      final_play->cfg.raw_file.has_filename = temp.has_filename;
      final_play->cfg.raw_file.format = temp.file_format;
      final_play->cfg.raw_file.has_format = temp.has_file_format;
      final_play->cfg.raw_file.channels = temp.channels;
      final_play->cfg.raw_file.wav_header = temp.is_wav;
      final_play->cfg.raw_file.has_wav_header = temp.has_is_wav;
      break;
    case AUDIO_BACKEND_TYPE_STDIN_OUT:
      final_play->cfg.stdout_out.channels = temp.channels;
      final_play->cfg.stdout_out.format = temp.file_format;
      final_play->cfg.stdout_out.wav_header = temp.is_wav;
      final_play->cfg.stdout_out.has_wav_header = temp.has_is_wav;
      break;
#if defined(ENABLE_WASAPI)
    case AUDIO_BACKEND_TYPE_WASAPI:
      final_play->cfg.wasapi.channels = temp.channels;
      snprintf(final_play->cfg.wasapi.device,
               sizeof(final_play->cfg.wasapi.device), "%s", temp.device);
      final_play->cfg.wasapi.has_device = temp.has_device;
      final_play->cfg.wasapi.format = temp.wasapi_format;
      final_play->cfg.wasapi.has_format = temp.has_wasapi_format;
      final_play->cfg.wasapi.exclusive = temp.exclusive;
      final_play->cfg.wasapi.has_exclusive = temp.has_exclusive;
      final_play->cfg.wasapi.polling = temp.polling;
      final_play->cfg.wasapi.has_polling = temp.has_polling;
      break;
#endif
#if defined(ENABLE_ASIO)
    case AUDIO_BACKEND_TYPE_ASIO:
      final_play->cfg.asio.channels = temp.channels;
      snprintf(final_play->cfg.asio.device, sizeof(final_play->cfg.asio.device),
               "%s", temp.device);
      final_play->cfg.asio.format = temp.asio_format;
      final_play->cfg.asio.has_format = temp.has_asio_format;
      break;
#endif
    default:
      break;
  }
}

/**
 * @brief Parses the top-level devices section from the configuration.
 *
 * Extracts global configuration fields like sample rate, chunk size, limits,
 * adjust options, and calls helpers to parse resampler, capture, and playback
 * settings.
 *
 * @param dev_obj The cJSON object representing the "devices" section.
 * @param config Pointer to the top-level configuration structure.
 * @param err Pointer to config_error_t to record errors.
 * @return 0 on success, or -1 on error (with err populated).
 */
static int parse_devices(const cJSON* dev_obj, dsp_config_t* config,
                         config_error_t* err) {
  if (!cJSON_IsObject(dev_obj)) {
    config_error_set(err, CONFIG_ERR_PARSE, "devices must be an object");
    return -1;
  }
  devices_config_t* dev = &config->devices;

  cJSON* item;

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "samplerate");
  if (cJSON_IsNumber(item)) {
    dev->samplerate = item->valueint > 0 ? (size_t)item->valueint : 0;
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "chunksize");
  if (cJSON_IsNumber(item)) {
    dev->chunksize = item->valueint > 0 ? (size_t)item->valueint : 0;
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "queuelimit");
  if (cJSON_IsNumber(item)) {
    dev->queuelimit = item->valueint;
    dev->has_queuelimit = (dev->queuelimit > 0);
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "enable_rate_adjust");
  if (cJSON_IsBool(item)) {
    dev->enable_rate_adjust = cJSON_IsTrue(item);
    dev->has_enable_rate_adjust = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "target_level");
  if (cJSON_IsNumber(item)) {
    dev->target_level = item->valueint;
    dev->has_target_level = (dev->target_level > 0);
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "adjust_period");
  if (cJSON_IsNumber(item)) {
    dev->adjust_period = item->valuedouble;
    dev->has_adjust_period = (dev->adjust_period > 0.0);
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "silence_threshold");
  if (cJSON_IsNumber(item)) {
    dev->silence_threshold = item->valuedouble;
    dev->has_silence_threshold = (dev->silence_threshold != 0.0);
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "silence_timeout");
  if (cJSON_IsNumber(item)) {
    dev->silence_timeout = item->valuedouble;
    dev->has_silence_timeout = (dev->silence_timeout > 0.0);
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "capture_samplerate");
  if (cJSON_IsNumber(item)) {
    dev->capture_samplerate = item->valueint > 0 ? (size_t)item->valueint : 0;
    dev->has_capture_samplerate = (dev->capture_samplerate > 0);
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "volume_ramp_time");
  if (cJSON_IsNumber(item)) {
    dev->volume_ramp_time = item->valuedouble;
    dev->has_volume_ramp_time = (dev->volume_ramp_time > 0.0);
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "volume_limit");
  if (cJSON_IsNumber(item)) {
    dev->volume_limit = item->valuedouble;
    dev->has_volume_limit = (dev->volume_limit > 0.0);
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "stop_on_rate_change");
  if (cJSON_IsBool(item)) {
    dev->stop_on_rate_change = cJSON_IsTrue(item);
    dev->has_stop_on_rate_change = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "rate_measure_interval");
  if (cJSON_IsNumber(item)) {
    dev->rate_measure_interval = item->valuedouble;
    dev->has_rate_measure_interval = (dev->rate_measure_interval > 0.0);
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "multithreaded");
  if (cJSON_IsBool(item)) {
    dev->multithreaded = cJSON_IsTrue(item);
    dev->has_multithreaded = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "worker_threads");
  if (cJSON_IsNumber(item)) {
    dev->worker_threads = item->valueint;
    dev->has_worker_threads = (dev->worker_threads > 0);
  }

  parse_resampler(cJSON_GetObjectItemCaseSensitive(dev_obj, "resampler"), dev);
  parse_capture(cJSON_GetObjectItemCaseSensitive(dev_obj, "capture"), dev);
  parse_playback(cJSON_GetObjectItemCaseSensitive(dev_obj, "playback"), dev);

  return 0;
}

/**
 * @brief Parses the pipeline steps from the configuration.
 *
 * Iterates through the pipeline array to extract step types (Filter, Mixer,
 * Processor), names, channels, and bypass flags.
 *
 * @param pipe_arr The cJSON array representing the "pipeline" section.
 * @param config Pointer to the top-level configuration structure.
 * @param err Pointer to config_error_t to record errors.
 * @return 0 on success, or -1 on error.
 */
static int parse_pipeline(const cJSON* pipe_arr, dsp_config_t* config,
                          config_error_t* err) {
  if (!cJSON_IsArray(pipe_arr)) {
    config_error_set(err, CONFIG_ERR_PARSE, "pipeline must be an array");
    return -1;
  }
  int size = cJSON_GetArraySize(pipe_arr);
  if (size == 0) return 0;

  config->pipeline = (pipeline_step_t*)calloc(size, sizeof(pipeline_step_t));
  if (!config->pipeline) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return -1;
  }
  config->pipeline_count = size;

  for (int s = 0; s < size; s++) {
    cJSON* step_obj = cJSON_GetArrayItem(pipe_arr, s);
    if (!cJSON_IsObject(step_obj)) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Pipeline step must be an object");
      return -1;
    }
    pipeline_step_t* step = &config->pipeline[s];

    cJSON* item;

    item = cJSON_GetObjectItemCaseSensitive(step_obj, "type");
    if (cJSON_IsString(item) && item->valuestring) {
      if (strcmp(item->valuestring, "Filter") == 0)
        step->type = PIPELINE_STEP_TYPE_FILTER;
      else if (strcmp(item->valuestring, "Mixer") == 0)
        step->type = PIPELINE_STEP_TYPE_MIXER;
      else if (strcmp(item->valuestring, "Processor") == 0)
        step->type = PIPELINE_STEP_TYPE_PROCESSOR;
    }

    item = cJSON_GetObjectItemCaseSensitive(step_obj, "name");
    if (cJSON_IsString(item) && item->valuestring) {
      strncpy(step->name, item->valuestring, sizeof(step->name) - 1);
      step->has_name = true;
    }

    item = cJSON_GetObjectItemCaseSensitive(step_obj, "channel");
    if (cJSON_IsNumber(item)) {
      step->channel = item->valueint;
      step->has_channel = true;
    }

    item = cJSON_GetObjectItemCaseSensitive(step_obj, "bypassed");
    if (cJSON_IsBool(item)) {
      step->bypassed = cJSON_IsTrue(item);
    }

    cJSON* names_arr = cJSON_GetObjectItemCaseSensitive(step_obj, "names");
    bool dummy;
    parse_labels_array(names_arr, &step->names, &step->names_count, &dummy);

    cJSON* channels_arr =
        cJSON_GetObjectItemCaseSensitive(step_obj, "channels");
    step->channels = parse_int_array(channels_arr, &step->channels_count);
  }
  return 0;
}

/**
 * @brief Parses mixers defined in the configuration.
 *
 * Iterates through the mixers object, parsing input/output channels and the
 * matrix mapping.
 *
 * @param mixers_obj The cJSON object containing mixer definitions.
 * @param config Pointer to the top-level configuration structure.
 * @param err Pointer to config_error_t to record errors.
 * @return 0 on success, or -1 on error.
 */
static int parse_mixers(const cJSON* mixers_obj, dsp_config_t* config,
                        config_error_t* err) {
  if (!cJSON_IsObject(mixers_obj)) {
    config_error_set(err, CONFIG_ERR_PARSE, "mixers must be an object");
    return -1;
  }
  int size = 0;
  cJSON* mixer_child = NULL;
  cJSON_ArrayForEach(mixer_child, mixers_obj) { size++; }
  if (size == 0) return 0;

  config->mixers =
      (named_mixer_config_t*)calloc(size, sizeof(named_mixer_config_t));
  if (!config->mixers) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return -1;
  }
  config->mixers_count = size;

  int m = 0;
  cJSON_ArrayForEach(mixer_child, mixers_obj) {
    named_mixer_config_t* nm = &config->mixers[m];
    strncpy(nm->name, mixer_child->string, sizeof(nm->name) - 1);

    if (!cJSON_IsObject(mixer_child)) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Mixer definition must be an object");
      return -1;
    }

    mixer_config_t* m_conf = &nm->mixer;

    cJSON* channels_obj =
        cJSON_GetObjectItemCaseSensitive(mixer_child, "channels");
    if (cJSON_IsObject(channels_obj)) {
      cJSON* in = cJSON_GetObjectItemCaseSensitive(channels_obj, "in");
      if (cJSON_IsNumber(in)) {
        m_conf->channels_in = in->valueint;
      }
      cJSON* out = cJSON_GetObjectItemCaseSensitive(channels_obj, "out");
      if (cJSON_IsNumber(out)) {
        m_conf->channels_out = out->valueint;
      }
    } else {
      cJSON* in = cJSON_GetObjectItemCaseSensitive(mixer_child, "channels_in");
      if (cJSON_IsNumber(in)) {
        m_conf->channels_in = in->valueint;
      }
      cJSON* out =
          cJSON_GetObjectItemCaseSensitive(mixer_child, "channels_out");
      if (cJSON_IsNumber(out)) {
        m_conf->channels_out = out->valueint;
      }
    }

    cJSON* mapping_arr =
        cJSON_GetObjectItemCaseSensitive(mixer_child, "mapping");
    if (cJSON_IsArray(mapping_arr)) {
      int map_size = cJSON_GetArraySize(mapping_arr);
      m_conf->mapping =
          (mixer_mapping_t*)calloc(map_size, sizeof(mixer_mapping_t));
      if (!m_conf->mapping) {
        config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
        return -1;
      }
      m_conf->mapping_count = map_size;

      for (int mp = 0; mp < map_size; mp++) {
        cJSON* map_el = cJSON_GetArrayItem(mapping_arr, mp);
        if (cJSON_IsObject(map_el)) {
          mixer_mapping_t* mapping = &m_conf->mapping[mp];

          cJSON* dest = cJSON_GetObjectItemCaseSensitive(map_el, "dest");
          if (cJSON_IsNumber(dest)) {
            mapping->dest = dest->valueint;
          }

          cJSON* mute = cJSON_GetObjectItemCaseSensitive(map_el, "mute");
          if (cJSON_IsBool(mute)) {
            mapping->mute = cJSON_IsTrue(mute);
          }

          cJSON* sources_arr =
              cJSON_GetObjectItemCaseSensitive(map_el, "sources");
          if (cJSON_IsArray(sources_arr)) {
            int src_size = cJSON_GetArraySize(sources_arr);
            mapping->sources =
                (mixer_source_t*)calloc(src_size, sizeof(mixer_source_t));
            if (!mapping->sources) {
              config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
              return -1;
            }
            mapping->sources_count = src_size;

            for (int s = 0; s < src_size; s++) {
              cJSON* src_el = cJSON_GetArrayItem(sources_arr, s);
              if (cJSON_IsObject(src_el)) {
                mixer_source_t* src = &mapping->sources[s];

                cJSON* chan =
                    cJSON_GetObjectItemCaseSensitive(src_el, "channel");
                if (cJSON_IsNumber(chan)) {
                  src->channel = chan->valueint;
                }

                cJSON* gain = cJSON_GetObjectItemCaseSensitive(src_el, "gain");
                if (cJSON_IsNumber(gain)) {
                  src->gain = gain->valuedouble;
                  src->has_gain = true;
                }

                cJSON* scale =
                    cJSON_GetObjectItemCaseSensitive(src_el, "scale");
                if (cJSON_IsString(scale) && scale->valuestring) {
                  if (strcasecmp(scale->valuestring, "Linear") == 0)
                    src->scale = GAIN_SCALE_LINEAR;
                  else
                    src->scale = GAIN_SCALE_DB;
                } else {
                  src->scale = GAIN_SCALE_DB;
                }

                cJSON* inv =
                    cJSON_GetObjectItemCaseSensitive(src_el, "inverted");
                if (cJSON_IsBool(inv)) {
                  src->inverted = cJSON_IsTrue(inv);
                }

                cJSON* smute = cJSON_GetObjectItemCaseSensitive(src_el, "mute");
                if (cJSON_IsBool(smute)) {
                  src->mute = cJSON_IsTrue(smute);
                }
              }
            }
          }
        }
      }
    }
    m++;
  }
  return 0;
}

/**
 * @brief Parses filters defined in the configuration.
 *
 * Parsers various filter types (Gain, Volume, Loudness, Biquad, Delay, Conv,
 * BiquadCombo, DiffEq, Dither, Limiter, LookaheadLimiter) and their specific
 * parameters.
 *
 * @param filters_obj The cJSON object containing filter definitions.
 * @param config Pointer to the top-level configuration structure.
 * @param err Pointer to config_error_t to record errors.
 * @return 0 on success, or -1 on error.
 */
static int parse_filters(const cJSON* filters_obj, dsp_config_t* config,
                         config_error_t* err) {
  if (!cJSON_IsObject(filters_obj)) {
    config_error_set(err, CONFIG_ERR_PARSE, "filters must be an object");
    return -1;
  }
  int size = 0;
  cJSON* filter_child = NULL;
  cJSON_ArrayForEach(filter_child, filters_obj) { size++; }
  if (size == 0) return 0;

  config->filters =
      (named_filter_config_t*)calloc(size, sizeof(named_filter_config_t));
  if (!config->filters) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return -1;
  }
  config->filters_count = size;

  int f = 0;
  cJSON_ArrayForEach(filter_child, filters_obj) {
    named_filter_config_t* nf = &config->filters[f];
    strncpy(nf->name, filter_child->string, sizeof(nf->name) - 1);

    if (!cJSON_IsObject(filter_child)) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Filter definition must be an object");
      return -1;
    }

    filter_config_t* f_conf = &nf->filter;

    cJSON* type = cJSON_GetObjectItemCaseSensitive(filter_child, "type");
    if (cJSON_IsString(type) && type->valuestring) {
      f_conf->type = filter_type_from_string(type->valuestring);
    }

    cJSON* params =
        cJSON_GetObjectItemCaseSensitive(filter_child, "parameters");
    if (cJSON_IsObject(params)) {
      cJSON* item;
      switch (f_conf->type) {
        case FILTER_TYPE_GAIN: {
          gain_parameters_t* gp = &f_conf->parameters.gain;
          item = cJSON_GetObjectItemCaseSensitive(params, "gain");
          if (cJSON_IsNumber(item)) {
            gp->gain = item->valuedouble;
            gp->has_gain = true;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "scale");
          if (cJSON_IsString(item) && item->valuestring) {
            if (strcasecmp(item->valuestring, "Linear") == 0)
              gp->scale = GAIN_SCALE_LINEAR;
            else
              gp->scale = GAIN_SCALE_DB;
          } else {
            gp->scale = GAIN_SCALE_DB;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "inverted");
          if (cJSON_IsBool(item)) {
            gp->inverted = cJSON_IsTrue(item);
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "mute");
          if (cJSON_IsBool(item)) {
            gp->mute = cJSON_IsTrue(item);
          }
          break;
        }
        case FILTER_TYPE_VOLUME: {
          volume_parameters_t* vp = &f_conf->parameters.volume;
          item = cJSON_GetObjectItemCaseSensitive(params, "ramp_time");
          if (cJSON_IsNumber(item)) {
            vp->ramp_time = item->valuedouble;
            vp->has_ramp_time = true;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "limit");
          if (cJSON_IsNumber(item)) {
            vp->limit = item->valuedouble;
            vp->has_limit = true;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "fader");
          if (item) {
            if (cJSON_IsString(item) && item->valuestring) {
              vp->fader = fader_from_string(item->valuestring);
            } else if (cJSON_IsNumber(item)) {
              vp->fader = (fader_t)item->valueint;
            }
          } else {
            vp->fader = FADER_MAIN;
          }
          break;
        }
        case FILTER_TYPE_LOUDNESS: {
          loudness_parameters_t* lp = &f_conf->parameters.loudness;
          item = cJSON_GetObjectItemCaseSensitive(params, "reference_level");
          if (cJSON_IsNumber(item)) {
            lp->reference_level = item->valuedouble;
            lp->has_reference_level = true;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "high_boost");
          if (cJSON_IsNumber(item)) {
            lp->high_boost = item->valuedouble;
            lp->has_high_boost = true;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "low_boost");
          if (cJSON_IsNumber(item)) {
            lp->low_boost = item->valuedouble;
            lp->has_low_boost = true;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "attenuate_mid");
          if (cJSON_IsBool(item)) {
            lp->attenuate_mid = cJSON_IsTrue(item);
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "fader");
          if (item) {
            if (cJSON_IsString(item) && item->valuestring) {
              lp->fader = fader_from_string(item->valuestring);
            } else if (cJSON_IsNumber(item)) {
              lp->fader = (fader_t)item->valueint;
            }
          } else {
            lp->fader = FADER_MAIN;
          }
          break;
        }
        case FILTER_TYPE_BIQUAD: {
          biquad_parameters_t* bp = &f_conf->parameters.biquad;
          item = cJSON_GetObjectItemCaseSensitive(params, "type");
          if (cJSON_IsString(item) && item->valuestring) {
            if (strcmp(item->valuestring, "Free") == 0)
              bp->type = BIQUAD_TYPE_FREE;
            else if (strcmp(item->valuestring, "Highpass") == 0)
              bp->type = BIQUAD_TYPE_HIGHPASS;
            else if (strcmp(item->valuestring, "Lowpass") == 0)
              bp->type = BIQUAD_TYPE_LOWPASS;
            else if (strcmp(item->valuestring, "HighpassFO") == 0)
              bp->type = BIQUAD_TYPE_HIGHPASS_FO;
            else if (strcmp(item->valuestring, "LowpassFO") == 0)
              bp->type = BIQUAD_TYPE_LOWPASS_FO;
            else if (strcmp(item->valuestring, "Highshelf") == 0)
              bp->type = BIQUAD_TYPE_HIGHSHELF;
            else if (strcmp(item->valuestring, "Lowshelf") == 0)
              bp->type = BIQUAD_TYPE_LOWSHELF;
            else if (strcmp(item->valuestring, "HighshelfFO") == 0)
              bp->type = BIQUAD_TYPE_HIGHSHELF_FO;
            else if (strcmp(item->valuestring, "LowshelfFO") == 0)
              bp->type = BIQUAD_TYPE_LOWSHELF_FO;
            else if (strcmp(item->valuestring, "Peaking") == 0)
              bp->type = BIQUAD_TYPE_PEAKING;
            else if (strcmp(item->valuestring, "Notch") == 0)
              bp->type = BIQUAD_TYPE_NOTCH;
            else if (strcmp(item->valuestring, "Bandpass") == 0)
              bp->type = BIQUAD_TYPE_BANDPASS;
            else if (strcmp(item->valuestring, "Allpass") == 0)
              bp->type = BIQUAD_TYPE_ALLPASS;
            else if (strcmp(item->valuestring, "AllpassFO") == 0)
              bp->type = BIQUAD_TYPE_ALLPASS_FO;
            else if (strcmp(item->valuestring, "GeneralNotch") == 0)
              bp->type = BIQUAD_TYPE_GENERAL_NOTCH;
            else if (strcmp(item->valuestring, "LinkwitzTransform") == 0)
              bp->type = BIQUAD_TYPE_LINKWITZ_TRANSFORM;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "freq");
          if (cJSON_IsNumber(item)) bp->freq = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "gain");
          if (cJSON_IsNumber(item)) bp->gain = item->valuedouble;

          item = cJSON_GetObjectItemCaseSensitive(params, "q");
          if (cJSON_IsNumber(item)) {
            bp->q = item->valuedouble;
            bp->steepness_type = STEEPNESS_TYPE_Q;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "bandwidth");
          if (cJSON_IsNumber(item)) {
            bp->bandwidth = item->valuedouble;
            bp->steepness_type = STEEPNESS_TYPE_BANDWIDTH;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "slope");
          if (cJSON_IsNumber(item)) {
            bp->slope = item->valuedouble;
            bp->steepness_type = STEEPNESS_TYPE_SLOPE;
          }

          item = cJSON_GetObjectItemCaseSensitive(params, "a1");
          if (cJSON_IsNumber(item)) bp->a1 = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "a2");
          if (cJSON_IsNumber(item)) bp->a2 = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "b0");
          if (cJSON_IsNumber(item)) bp->b0 = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "b1");
          if (cJSON_IsNumber(item)) bp->b1 = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "b2");
          if (cJSON_IsNumber(item)) bp->b2 = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "freq_z");
          if (cJSON_IsNumber(item)) bp->freq_notch = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "freq_p");
          if (cJSON_IsNumber(item)) bp->freq_pole = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "q_p");
          if (cJSON_IsNumber(item)) bp->q_p = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "normalize_at_dc");
          if (cJSON_IsBool(item)) bp->normalize_at_dc = cJSON_IsTrue(item);
          item = cJSON_GetObjectItemCaseSensitive(params, "freq_act");
          if (cJSON_IsNumber(item)) bp->freq_act = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "q_act");
          if (cJSON_IsNumber(item)) bp->q_act = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "freq_target");
          if (cJSON_IsNumber(item)) bp->freq_target = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "q_target");
          if (cJSON_IsNumber(item)) bp->q_target = item->valuedouble;
          break;
        }
        case FILTER_TYPE_DELAY: {
          delay_parameters_t* dp = &f_conf->parameters.delay;
          item = cJSON_GetObjectItemCaseSensitive(params, "delay");
          if (cJSON_IsNumber(item)) dp->delay = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "unit");
          if (cJSON_IsString(item) && item->valuestring) {
            if (strcmp(item->valuestring, "ms") == 0)
              dp->unit = DELAY_UNIT_MS;
            else if (strcmp(item->valuestring, "us") == 0)
              dp->unit = DELAY_UNIT_US;
            else if (strcmp(item->valuestring, "samples") == 0)
              dp->unit = DELAY_UNIT_SAMPLES;
            else if (strcmp(item->valuestring, "mm") == 0)
              dp->unit = DELAY_UNIT_MM;
          } else {
            dp->unit = DELAY_UNIT_MS;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "subsample");
          if (cJSON_IsBool(item)) dp->subsample = cJSON_IsTrue(item);
          break;
        }
        case FILTER_TYPE_CONV: {
          conv_parameters_t* cp = &f_conf->parameters.conv;
          item = cJSON_GetObjectItemCaseSensitive(params, "type");
          if (cJSON_IsString(item) && item->valuestring) {
            if (strcmp(item->valuestring, "Values") == 0)
              cp->type = CONV_TYPE_VALUES;
            else if (strcmp(item->valuestring, "Wav") == 0)
              cp->type = CONV_TYPE_WAV;
            else if (strcmp(item->valuestring, "Raw") == 0)
              cp->type = CONV_TYPE_RAW;
            else
              cp->type = CONV_TYPE_DUMMY;
          }
          cJSON* val_arr = cJSON_GetObjectItemCaseSensitive(params, "values");
          cp->values = parse_double_array(val_arr, &cp->values_count);
          item = cJSON_GetObjectItemCaseSensitive(params, "filename");
          if (cJSON_IsString(item) && item->valuestring) {
            strncpy(cp->filename, item->valuestring, sizeof(cp->filename) - 1);
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "format");
          if (cJSON_IsString(item) && item->valuestring) {
            strncpy(cp->format, item->valuestring, sizeof(cp->format) - 1);
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "channel");
          if (cJSON_IsNumber(item)) cp->channel = item->valueint;
          item = cJSON_GetObjectItemCaseSensitive(params, "length");
          if (cJSON_IsNumber(item)) cp->length = item->valueint;
          item = cJSON_GetObjectItemCaseSensitive(params, "skip_bytes_lines");
          if (cJSON_IsNumber(item)) cp->skip_bytes_lines = item->valueint;
          item = cJSON_GetObjectItemCaseSensitive(params, "read_bytes_lines");
          if (cJSON_IsNumber(item)) cp->read_bytes_lines = item->valueint;
          break;
        }
        case FILTER_TYPE_BIQUAD_COMBO: {
          biquad_combo_parameters_t* bcp = &f_conf->parameters.biquad_combo;
          item = cJSON_GetObjectItemCaseSensitive(params, "type");
          if (cJSON_IsString(item) && item->valuestring) {
            if (strcmp(item->valuestring, "ButterworthHighpass") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS;
            else if (strcmp(item->valuestring, "ButterworthLowpass") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS;
            else if (strcmp(item->valuestring, "LinkwitzRileyHighpass") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS;
            else if (strcmp(item->valuestring, "LinkwitzRileyLowpass") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS;
            else if (strcmp(item->valuestring, "Tilt") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_TILT;
            else if (strcmp(item->valuestring, "FivePointPEQ") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_FIVE_POINT_PEQ;
            else if (strcmp(item->valuestring, "GraphicEqualizer") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "freq");
          if (cJSON_IsNumber(item)) {
            bcp->freq = item->valuedouble;
            bcp->has_freq = true;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "freq_min");
          if (cJSON_IsNumber(item)) {
            bcp->freq_min = item->valuedouble;
            bcp->has_freq_min = true;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "freq_max");
          if (cJSON_IsNumber(item)) {
            bcp->freq_max = item->valuedouble;
            bcp->has_freq_max = true;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "order");
          if (cJSON_IsNumber(item)) {
            bcp->order = item->valueint;
            bcp->has_order = true;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "gain");
          if (cJSON_IsNumber(item)) {
            bcp->gain = item->valuedouble;
            bcp->has_gain = true;
          }
#define PARSE_COMBO_DOUBLE(name, field)                   \
  item = cJSON_GetObjectItemCaseSensitive(params, #name); \
  if (cJSON_IsNumber(item)) {                             \
    bcp->field = item->valuedouble;                       \
    bcp->has_##field = true;                              \
  }
          PARSE_COMBO_DOUBLE(fls, fls)
          PARSE_COMBO_DOUBLE(qls, qls)
          PARSE_COMBO_DOUBLE(gls, gls)
          PARSE_COMBO_DOUBLE(fp1, fp1)
          PARSE_COMBO_DOUBLE(qp1, qp1)
          PARSE_COMBO_DOUBLE(gp1, gp1)
          PARSE_COMBO_DOUBLE(fp2, fp2)
          PARSE_COMBO_DOUBLE(qp2, qp2)
          PARSE_COMBO_DOUBLE(gp2, gp2)
          PARSE_COMBO_DOUBLE(fp3, fp3)
          PARSE_COMBO_DOUBLE(qp3, qp3)
          PARSE_COMBO_DOUBLE(gp3, gp3)
          PARSE_COMBO_DOUBLE(fhs, fhs)
          PARSE_COMBO_DOUBLE(qhs, qhs)
          PARSE_COMBO_DOUBLE(ghs, ghs)
#undef PARSE_COMBO_DOUBLE

          cJSON* gains_arr = cJSON_GetObjectItemCaseSensitive(params, "gains");
          bcp->gains = parse_double_array(gains_arr, &bcp->gains_count);
          break;
        }
        case FILTER_TYPE_DIFF_EQ: {
          diff_eq_parameters_t* dep = &f_conf->parameters.diff_eq;
          cJSON* a_arr = cJSON_GetObjectItemCaseSensitive(params, "a");
          dep->a = parse_double_array(a_arr, &dep->a_count);
          cJSON* b_arr = cJSON_GetObjectItemCaseSensitive(params, "b");
          dep->b = parse_double_array(b_arr, &dep->b_count);
          break;
        }
        case FILTER_TYPE_DITHER: {
          dither_parameters_t* dp = &f_conf->parameters.dither;
          item = cJSON_GetObjectItemCaseSensitive(params, "type");
          if (cJSON_IsString(item) && item->valuestring) {
            if (strcmp(item->valuestring, "None") == 0)
              dp->type = DITHER_TYPE_NONE;
            else if (strcmp(item->valuestring, "Flat") == 0)
              dp->type = DITHER_TYPE_FLAT;
            else if (strcmp(item->valuestring, "Highpass") == 0)
              dp->type = DITHER_TYPE_HIGHPASS;
            else if (strcmp(item->valuestring, "Fweighted441") == 0)
              dp->type = DITHER_TYPE_FWEIGHTED_441;
            else if (strcmp(item->valuestring, "FweightedLong441") == 0)
              dp->type = DITHER_TYPE_FWEIGHTED_LONG_441;
            else if (strcmp(item->valuestring, "FweightedShort441") == 0)
              dp->type = DITHER_TYPE_FWEIGHTED_SHORT_441;
            else if (strcmp(item->valuestring, "Gesemann441") == 0)
              dp->type = DITHER_TYPE_GESEMANN_441;
            else if (strcmp(item->valuestring, "Gesemann48") == 0)
              dp->type = DITHER_TYPE_GESEMANN_48;
            else if (strcmp(item->valuestring, "Lipshitz441") == 0)
              dp->type = DITHER_TYPE_LIPSHITZ_441;
            else if (strcmp(item->valuestring, "LipshitzLong441") == 0)
              dp->type = DITHER_TYPE_LIPSHITZ_LONG_441;
            else if (strcmp(item->valuestring, "Shibata441") == 0)
              dp->type = DITHER_TYPE_SHIBATA_441;
            else if (strcmp(item->valuestring, "ShibataHigh441") == 0)
              dp->type = DITHER_TYPE_SHIBATA_HIGH_441;
            else if (strcmp(item->valuestring, "ShibataLow441") == 0)
              dp->type = DITHER_TYPE_SHIBATA_LOW_441;
            else if (strcmp(item->valuestring, "Shibata48") == 0)
              dp->type = DITHER_TYPE_SHIBATA_48;
            else if (strcmp(item->valuestring, "ShibataHigh48") == 0)
              dp->type = DITHER_TYPE_SHIBATA_HIGH_48;
            else if (strcmp(item->valuestring, "ShibataLow48") == 0)
              dp->type = DITHER_TYPE_SHIBATA_LOW_48;
            else if (strcmp(item->valuestring, "Shibata882") == 0)
              dp->type = DITHER_TYPE_SHIBATA_882;
            else if (strcmp(item->valuestring, "ShibataLow882") == 0)
              dp->type = DITHER_TYPE_SHIBATA_LOW_882;
            else if (strcmp(item->valuestring, "Shibata96") == 0)
              dp->type = DITHER_TYPE_SHIBATA_96;
            else if (strcmp(item->valuestring, "ShibataLow96") == 0)
              dp->type = DITHER_TYPE_SHIBATA_LOW_96;
            else if (strcmp(item->valuestring, "Shibata192") == 0)
              dp->type = DITHER_TYPE_SHIBATA_192;
            else if (strcmp(item->valuestring, "ShibataLow192") == 0)
              dp->type = DITHER_TYPE_SHIBATA_LOW_192;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "bits");
          if (cJSON_IsNumber(item)) dp->bits = item->valueint;
          item = cJSON_GetObjectItemCaseSensitive(params, "amplitude");
          if (cJSON_IsNumber(item)) {
            dp->amplitude = item->valuedouble;
            dp->has_amplitude = true;
          }
          break;
        }
        case FILTER_TYPE_LIMITER: {
          limiter_parameters_t* lp = &f_conf->parameters.limiter;
          item = cJSON_GetObjectItemCaseSensitive(params, "clip_limit");
          if (cJSON_IsNumber(item)) lp->clip_limit = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "soft_clip");
          if (cJSON_IsBool(item)) lp->soft_clip = cJSON_IsTrue(item);
          break;
        }
        case FILTER_TYPE_LOOKAHEAD_LIMITER: {
          lookahead_limiter_parameters_t* llp =
              &f_conf->parameters.lookahead_limiter;
          item = cJSON_GetObjectItemCaseSensitive(params, "limit");
          if (cJSON_IsNumber(item)) llp->limit = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "attack");
          if (cJSON_IsNumber(item)) llp->attack = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "release");
          if (cJSON_IsNumber(item)) llp->release = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "unit");
          if (cJSON_IsString(item) && item->valuestring) {
            if (strcmp(item->valuestring, "ms") == 0)
              llp->unit = DELAY_UNIT_MS;
            else if (strcmp(item->valuestring, "us") == 0)
              llp->unit = DELAY_UNIT_US;
            else if (strcmp(item->valuestring, "samples") == 0)
              llp->unit = DELAY_UNIT_SAMPLES;
            else if (strcmp(item->valuestring, "mm") == 0)
              llp->unit = DELAY_UNIT_MM;
          } else {
            llp->unit = DELAY_UNIT_MS;
          }
          break;
        }
        default:
          break;
      }
    }
    f++;
  }
  return 0;
}

/**
 * @brief Parses processors defined in the configuration.
 *
 * Parses processor parameters based on the type (Compressor, NoiseGate, RACE).
 *
 * @param processors_obj The cJSON object containing processor definitions.
 * @param config Pointer to the top-level configuration structure.
 * @param err Pointer to config_error_t to record errors.
 * @return 0 on success, or -1 on error.
 */
static int parse_processors(const cJSON* processors_obj, dsp_config_t* config,
                            config_error_t* err) {
  if (!cJSON_IsObject(processors_obj)) {
    config_error_set(err, CONFIG_ERR_PARSE, "processors must be an object");
    return -1;
  }
  int size = 0;
  cJSON* proc_child = NULL;
  cJSON_ArrayForEach(proc_child, processors_obj) { size++; }
  if (size == 0) return 0;

  config->processors =
      (named_processor_config_t*)calloc(size, sizeof(named_processor_config_t));
  if (!config->processors) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return -1;
  }
  config->processors_count = size;

  int p = 0;
  cJSON_ArrayForEach(proc_child, processors_obj) {
    named_processor_config_t* np = &config->processors[p];
    strncpy(np->name, proc_child->string, sizeof(np->name) - 1);

    if (!cJSON_IsObject(proc_child)) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Processor definition must be an object");
      return -1;
    }

    processor_config_t* p_conf = &np->processor;

    cJSON* type = cJSON_GetObjectItemCaseSensitive(proc_child, "type");
    if (cJSON_IsString(type) && type->valuestring) {
      p_conf->type = processor_type_from_string(type->valuestring);
    }

    cJSON* params = cJSON_GetObjectItemCaseSensitive(proc_child, "parameters");
    if (cJSON_IsObject(params)) {
      cJSON* item;
      switch (p_conf->type) {
        case PROCESSOR_TYPE_COMPRESSOR: {
          compressor_parameters_t* cp = &p_conf->parameters.compressor;

          item = cJSON_GetObjectItemCaseSensitive(params, "channels");
          if (cJSON_IsNumber(item)) cp->channels = item->valueint;

          item = cJSON_GetObjectItemCaseSensitive(params, "attack");
          if (cJSON_IsNumber(item)) cp->attack = item->valuedouble;

          item = cJSON_GetObjectItemCaseSensitive(params, "release");
          if (cJSON_IsNumber(item)) cp->release = item->valuedouble;

          item = cJSON_GetObjectItemCaseSensitive(params, "threshold");
          if (cJSON_IsNumber(item)) cp->threshold = item->valuedouble;

          item = cJSON_GetObjectItemCaseSensitive(params, "factor");
          if (cJSON_IsNumber(item)) cp->factor = item->valuedouble;

          item = cJSON_GetObjectItemCaseSensitive(params, "makeup_gain");
          if (cJSON_IsNumber(item)) {
            cp->makeup_gain = item->valuedouble;
            cp->has_makeup_gain = true;
          }

          item = cJSON_GetObjectItemCaseSensitive(params, "soft_clip");
          if (cJSON_IsBool(item)) cp->soft_clip = cJSON_IsTrue(item);

          item = cJSON_GetObjectItemCaseSensitive(params, "clip_limit");
          if (cJSON_IsNumber(item)) {
            cp->clip_limit = item->valuedouble;
            cp->has_clip_limit = true;
          }

          cJSON* mon_arr =
              cJSON_GetObjectItemCaseSensitive(params, "monitor_channels");
          cp->monitor_channels =
              parse_int_array(mon_arr, &cp->monitor_channels_count);

          cJSON* proc_arr =
              cJSON_GetObjectItemCaseSensitive(params, "process_channels");
          cp->process_channels =
              parse_int_array(proc_arr, &cp->process_channels_count);
          break;
        }
        case PROCESSOR_TYPE_NOISE_GATE: {
          noise_gate_parameters_t* ng = &p_conf->parameters.noise_gate;

          item = cJSON_GetObjectItemCaseSensitive(params, "channels");
          if (cJSON_IsNumber(item)) ng->channels = item->valueint;

          item = cJSON_GetObjectItemCaseSensitive(params, "attack");
          if (cJSON_IsNumber(item)) ng->attack = item->valuedouble;

          item = cJSON_GetObjectItemCaseSensitive(params, "release");
          if (cJSON_IsNumber(item)) ng->release = item->valuedouble;

          item = cJSON_GetObjectItemCaseSensitive(params, "threshold");
          if (cJSON_IsNumber(item)) ng->threshold = item->valuedouble;

          item = cJSON_GetObjectItemCaseSensitive(params, "attenuation");
          if (cJSON_IsNumber(item)) ng->attenuation = item->valuedouble;

          cJSON* mon_arr =
              cJSON_GetObjectItemCaseSensitive(params, "monitor_channels");
          ng->monitor_channels =
              parse_int_array(mon_arr, &ng->monitor_channels_count);

          cJSON* proc_arr =
              cJSON_GetObjectItemCaseSensitive(params, "process_channels");
          ng->process_channels =
              parse_int_array(proc_arr, &ng->process_channels_count);
          break;
        }
        case PROCESSOR_TYPE_RACE: {
          race_parameters_t* rp = &p_conf->parameters.race;

          item = cJSON_GetObjectItemCaseSensitive(params, "channels");
          if (cJSON_IsNumber(item)) rp->channels = item->valueint;

          item = cJSON_GetObjectItemCaseSensitive(params, "channel_a");
          if (cJSON_IsNumber(item)) rp->channel_a = item->valueint;

          item = cJSON_GetObjectItemCaseSensitive(params, "channel_b");
          if (cJSON_IsNumber(item)) rp->channel_b = item->valueint;

          item = cJSON_GetObjectItemCaseSensitive(params, "delay");
          if (cJSON_IsNumber(item)) rp->delay = item->valuedouble;

          item = cJSON_GetObjectItemCaseSensitive(params, "subsample_delay");
          if (cJSON_IsBool(item)) {
            rp->subsample_delay = cJSON_IsTrue(item);
            rp->has_subsample_delay = true;
          }

          item = cJSON_GetObjectItemCaseSensitive(params, "delay_unit");
          if (cJSON_IsString(item) && item->valuestring) {
            if (strcmp(item->valuestring, "ms") == 0)
              rp->delay_unit = DELAY_UNIT_MS;
            else if (strcmp(item->valuestring, "us") == 0)
              rp->delay_unit = DELAY_UNIT_US;
            else if (strcmp(item->valuestring, "samples") == 0)
              rp->delay_unit = DELAY_UNIT_SAMPLES;
            else if (strcmp(item->valuestring, "mm") == 0)
              rp->delay_unit = DELAY_UNIT_MM;
            rp->has_delay_unit = true;
          }

          item = cJSON_GetObjectItemCaseSensitive(params, "attenuation");
          if (cJSON_IsNumber(item)) rp->attenuation = item->valuedouble;
          break;
        }
      }
    }
    p++;
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

  cJSON* root = cJSON_Parse(json);
  if (!root) {
    free(config);
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to parse JSON (syntax error or invalid JSON)");
    return -1;
  }

  cJSON* devices_obj = cJSON_GetObjectItemCaseSensitive(root, "devices");
  if (!devices_obj) {
    cJSON_Delete(root);
    free(config);
    config_error_set(err, CONFIG_ERR_PARSE, "Config must contain 'devices'");
    return -1;
  }

  if (parse_devices(devices_obj, config, err) != 0) {
    cJSON_Delete(root);
    dsp_config_free(config);
    return -1;
  }

  cJSON* pipeline_arr = cJSON_GetObjectItemCaseSensitive(root, "pipeline");
  if (pipeline_arr) {
    if (parse_pipeline(pipeline_arr, config, err) != 0) {
      cJSON_Delete(root);
      dsp_config_free(config);
      return -1;
    }
  }

  cJSON* mixers_obj = cJSON_GetObjectItemCaseSensitive(root, "mixers");
  if (mixers_obj) {
    if (parse_mixers(mixers_obj, config, err) != 0) {
      cJSON_Delete(root);
      dsp_config_free(config);
      return -1;
    }
  }

  cJSON* filters_obj = cJSON_GetObjectItemCaseSensitive(root, "filters");
  if (filters_obj) {
    if (parse_filters(filters_obj, config, err) != 0) {
      cJSON_Delete(root);
      dsp_config_free(config);
      return -1;
    }
  }

  cJSON* processors_obj = cJSON_GetObjectItemCaseSensitive(root, "processors");
  if (processors_obj) {
    if (parse_processors(processors_obj, config, err) != 0) {
      cJSON_Delete(root);
      dsp_config_free(config);
      return -1;
    }
  }

  cJSON_Delete(root);

  /* Validate the populated configuration structure.
   * This checks schema constraints and traces channel flows through the
   * pipeline to catch configuration inconsistencies before return. */
  if (dsp_config_validate(config, err) != 0) {
    dsp_config_free(config);
    return -1;
  }

  *out_config = config;
  return 0;
}
