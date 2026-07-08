#ifndef CLIB_CONFIG_ENGINE_CONFIG_TYPES_H
#define CLIB_CONFIG_ENGINE_CONFIG_TYPES_H

// Standalone Engine Configuration and API Types

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config_error.h"
#include "resampler_config_types.h"

/// Engine processing state.
typedef enum {
  PROCESSING_STATE_INACTIVE = 0,
  PROCESSING_STATE_STARTING = 1,
  PROCESSING_STATE_RUNNING = 2,
  PROCESSING_STATE_PAUSED = 3,
  PROCESSING_STATE_STALLED = 4
} processing_state_t;

uint8_t processing_state_to_raw_byte(processing_state_t state);
processing_state_t processing_state_from_raw_byte(uint8_t raw_byte);
const char* processing_state_to_string(processing_state_t state);
processing_state_t processing_state_from_string(const char* str);

/// Why the engine stopped.
typedef enum {
  STOP_REASON_NONE = 0,
  STOP_REASON_DONE,
  STOP_REASON_CAPTURE_ERROR,
  STOP_REASON_PLAYBACK_ERROR,
  STOP_REASON_CAPTURE_FORMAT_CHANGE,
  STOP_REASON_PLAYBACK_FORMAT_CHANGE,
  STOP_REASON_UNKNOWN_ERROR
} processing_stop_reason_type_t;

typedef struct {
  processing_stop_reason_type_t type;
  char message[256];
  int format_change_rate;
} processing_stop_reason_t;

typedef struct {
  processing_state_t state;
  processing_stop_reason_t stop_reason;
} state_update_t;

typedef struct {
  char name[256];
} audio_device_t;

typedef enum {
  AUDIO_BACKEND_ERR_CONFIG_PARSE = 0,
  AUDIO_BACKEND_ERR_COMMAND_SEND,
  AUDIO_BACKEND_ERR_INVALID_SAMPLERATE,
  AUDIO_BACKEND_ERR_SPECTRUM_COMPUTE,
  AUDIO_BACKEND_ERR_ENGINE_NOT_RUNNING,
  AUDIO_BACKEND_ERR_BUFFER_EMPTY,
  AUDIO_BACKEND_ERR_DEVICE_NOT_FOUND,
  AUDIO_BACKEND_ERR_DEVICE_BUSY
} audio_backend_error_type_t;

typedef struct {
  audio_backend_error_type_t type;
  char message[256];
} audio_backend_error_t;

void audio_backend_error_description(const audio_backend_error_t* err,
                                     char* out_buf, size_t buf_len);

typedef struct {
  double* playback_rms;
  double* playback_peak;
  double* capture_rms;
  double* capture_peak;
  size_t playback_channels;
  size_t capture_channels;
} vu_levels_t;

typedef struct {
  double* frequencies;
  double* magnitudes;
  size_t count;
} spectrum_t;

typedef struct {
  double** channels;
  size_t channels_count;
  size_t frames;
} audio_samples_t;

// MARK: - Capability data model
#if defined(ENABLE_COREAUDIO)
typedef enum {
  COREAUDIO_SAMPLE_FORMAT_S16 = 0,
  COREAUDIO_SAMPLE_FORMAT_S24,
  COREAUDIO_SAMPLE_FORMAT_S32,
  COREAUDIO_SAMPLE_FORMAT_F32,
  COREAUDIO_SAMPLE_FORMAT_INVALID = -1
} coreaudio_sample_format_t;

const char* coreaudio_sample_format_to_string(coreaudio_sample_format_t fmt);
coreaudio_sample_format_t coreaudio_sample_format_from_string(const char* str);
#endif  // ENABLE_COREAUDIO

typedef struct {
  int samplerate;
  char** formats;
  size_t formats_count;
} samplerate_capability_t;

typedef struct {
  int channels;
  samplerate_capability_t* samplerates;
  size_t samplerates_count;
} channel_capability_t;

typedef struct {
  channel_capability_t* capabilities;
  size_t capabilities_count;
} device_capability_set_t;

typedef struct {
  char name[256];
  device_capability_set_t* capability_sets;
  size_t capability_sets_count;
} audio_device_descriptor_t;

// MARK: - Device Config Models

/// Audio I/O backend.
typedef enum {
#if defined(ENABLE_COREAUDIO)
  AUDIO_BACKEND_TYPE_CORE_AUDIO = 0,
#endif
#if defined(ENABLE_ALSA)
  AUDIO_BACKEND_TYPE_ALSA = 1,
#endif
#if defined(ENABLE_PULSE)
  AUDIO_BACKEND_TYPE_PULSE_AUDIO = 2,
#endif
#if defined(ENABLE_PIPEWIRE)
  AUDIO_BACKEND_TYPE_PIPEWIRE = 3,
#endif
#if defined(ENABLE_WASAPI)
  AUDIO_BACKEND_TYPE_WASAPI = 4,
#endif
#if defined(ENABLE_ASIO)
  AUDIO_BACKEND_TYPE_ASIO = 8,
#endif
#if defined(ENABLE_JACK)
  AUDIO_BACKEND_TYPE_JACK = 9,
#endif
#if defined(ENABLE_BLUEZ)
  AUDIO_BACKEND_TYPE_BLUEZ = 10,
#endif
  AUDIO_BACKEND_TYPE_FILE = 5,
  AUDIO_BACKEND_TYPE_STDIN_OUT = 6,
  AUDIO_BACKEND_TYPE_GENERATOR = 7,
  AUDIO_BACKEND_TYPE_INVALID = -1
} audio_backend_type_t;

const char* audio_backend_type_to_string(audio_backend_type_t type);
audio_backend_type_t audio_backend_type_from_string(const char* str);

typedef enum {
  SDM_FILTER_CLANS4 = 0,
  SDM_FILTER_SDM4,
  SDM_FILTER_CLANS5,
  SDM_FILTER_SDM5,
  SDM_FILTER_CLANS6,
  SDM_FILTER_SDM6,
  SDM_FILTER_CLANS7,
  SDM_FILTER_SDM7,
  SDM_FILTER_CLANS8,
  SDM_FILTER_SDM8,
  SDM_FILTER_INVALID = -1
} sdm_filter_t;

const char* sdm_filter_to_string(sdm_filter_t filter);
sdm_filter_t sdm_filter_from_string(const char* str);

#if defined(ENABLE_ALSA)
typedef enum {
  ALSA_SAMPLE_FORMAT_S16_LE = 0,
  ALSA_SAMPLE_FORMAT_S24_3_LE,
  ALSA_SAMPLE_FORMAT_S24_4_LE,
  ALSA_SAMPLE_FORMAT_S32_LE,
  ALSA_SAMPLE_FORMAT_F32_LE,
  ALSA_SAMPLE_FORMAT_F64_LE,
  ALSA_SAMPLE_FORMAT_INVALID = -1
} alsa_sample_format_t;

const char* alsa_sample_format_to_string(alsa_sample_format_t fmt);
alsa_sample_format_t alsa_sample_format_from_string(const char* str);
#endif

#if defined(ENABLE_WASAPI)
typedef enum {
  WASAPI_SAMPLE_FORMAT_S16 = 0,
  WASAPI_SAMPLE_FORMAT_S24,
  WASAPI_SAMPLE_FORMAT_S32,
  WASAPI_SAMPLE_FORMAT_F32,
  WASAPI_SAMPLE_FORMAT_INVALID = -1
} wasapi_sample_format_t;

const char* wasapi_sample_format_to_string(wasapi_sample_format_t fmt);
wasapi_sample_format_t wasapi_sample_format_from_string(const char* str);
#endif

#if defined(ENABLE_ASIO)
typedef enum {
  ASIO_SAMPLE_FORMAT_S16_LE = 0,
  ASIO_SAMPLE_FORMAT_S24_3_LE,
  ASIO_SAMPLE_FORMAT_S24_4_LE,
  ASIO_SAMPLE_FORMAT_S32_LE,
  ASIO_SAMPLE_FORMAT_F32_LE,
  ASIO_SAMPLE_FORMAT_F64_LE,
  ASIO_SAMPLE_FORMAT_INVALID = -1
} asio_sample_format_t;

const char* asio_sample_format_to_string(asio_sample_format_t fmt);
asio_sample_format_t asio_sample_format_from_string(const char* str);
#endif

typedef enum {
  BINARY_SAMPLE_FORMAT_S16_LE = 0,
  BINARY_SAMPLE_FORMAT_S24_3_LE,
  BINARY_SAMPLE_FORMAT_S24_4_RJ_LE,
  BINARY_SAMPLE_FORMAT_S24_4_LJ_LE,
  BINARY_SAMPLE_FORMAT_S32_LE,
  BINARY_SAMPLE_FORMAT_F32_LE,
  BINARY_SAMPLE_FORMAT_F64_LE,
  BINARY_SAMPLE_FORMAT_INVALID = -1
} binary_sample_format_t;

const char* binary_sample_format_to_string(binary_sample_format_t fmt);
binary_sample_format_t binary_sample_format_from_string(const char* str);

typedef enum {
  SIGNAL_TYPE_SINE = 0,
  SIGNAL_TYPE_SQUARE,
  SIGNAL_TYPE_WHITE_NOISE,
  SIGNAL_TYPE_INVALID = -1
} signal_type_t;

const char* signal_type_to_string(signal_type_t type);
signal_type_t signal_type_from_string(const char* str);

typedef struct {
  signal_type_t type;
  double frequency;
  double level;
} generator_signal_t;

#if defined(ENABLE_COREAUDIO)
typedef struct {
  int channels;
  char device[256];
  bool has_device;
  coreaudio_sample_format_t format;
  bool has_format;
  bool bypass_dop;
  bool has_bypass_dop;
  double dop_cutoff_hz;
  bool has_dop_cutoff_hz;
} coreaudio_capture_config_t;

typedef struct {
  int channels;
  char device[256];
  bool has_device;
  coreaudio_sample_format_t format;
  bool has_format;
  bool exclusive;
  bool has_exclusive;
  bool output_dop;
  bool has_output_dop;
  sdm_filter_t dop_encoder_filter;
  bool has_dop_encoder_filter;
} coreaudio_playback_config_t;
#endif

#if defined(ENABLE_ALSA)
typedef struct {
  int channels;
  char device[256];
  alsa_sample_format_t format;
  bool has_format;
  bool stop_on_inactive;
  bool has_stop_on_inactive;
  char link_volume_control[256];
  bool has_link_volume_control;
  char link_mute_control[256];
  bool has_link_mute_control;
} alsa_capture_config_t;

typedef struct {
  int channels;
  char device[256];
  alsa_sample_format_t format;
  bool has_format;
} alsa_playback_config_t;
#endif

#if defined(ENABLE_PULSE)
typedef struct {
  int channels;
  char device[256];
} pulse_capture_config_t;

typedef struct {
  int channels;
  char device[256];
} pulse_playback_config_t;
#endif

#if defined(ENABLE_PIPEWIRE)
typedef struct {
  int channels;
  char device[256];
  bool has_device;
  char node_name[256];
  bool has_node_name;
  char node_description[256];
  bool has_node_description;
  char node_group_name[256];
  bool has_node_group_name;
  char autoconnect_to[256];
  bool has_autoconnect_to;
} pipewire_capture_config_t;

typedef struct {
  int channels;
  char device[256];
  bool has_device;
  char node_name[256];
  bool has_node_name;
  char node_description[256];
  bool has_node_description;
  char node_group_name[256];
  bool has_node_group_name;
  char autoconnect_to[256];
  bool has_autoconnect_to;
} pipewire_playback_config_t;
#endif

#if defined(ENABLE_JACK)
typedef struct {
  int channels;
  char device[256];
} jack_capture_config_t;

typedef struct {
  int channels;
  char device[256];
} jack_playback_config_t;
#endif

typedef struct {
  int channels;
  binary_sample_format_t format;
  int extra_samples;
  bool has_extra_samples;
  int skip_bytes;
  bool has_skip_bytes;
  int read_bytes;
  bool has_read_bytes;
} stdin_capture_config_t;

typedef struct {
  int channels;
  binary_sample_format_t format;
  bool wav_header;
  bool has_wav_header;
} stdout_playback_config_t;

#if defined(ENABLE_WASAPI)
typedef struct {
  int channels;
  char device[256];
  bool has_device;
  wasapi_sample_format_t format;
  bool has_format;
  bool exclusive;
  bool has_exclusive;
  bool loopback;
  bool has_loopback;
  bool polling;
  bool has_polling;
} wasapi_capture_config_t;

typedef struct {
  int channels;
  char device[256];
  bool has_device;
  wasapi_sample_format_t format;
  bool has_format;
  bool exclusive;
  bool has_exclusive;
  bool polling;
  bool has_polling;
} wasapi_playback_config_t;
#endif

#if defined(ENABLE_ASIO)
typedef struct {
  int channels;
  char device[256];
  asio_sample_format_t format;
  bool has_format;
} asio_capture_config_t;

typedef struct {
  int channels;
  char device[256];
  asio_sample_format_t format;
  bool has_format;
} asio_playback_config_t;
#endif

#if defined(ENABLE_BLUEZ)
typedef struct {
  char service[256];
  bool has_service;
  char dbus_path[256];
  bool has_dbus_path;
  binary_sample_format_t format;
  int channels;
} bluez_capture_config_t;
#endif

typedef struct {
  char filename[512];
  bool has_filename;
  int extra_samples;
  bool has_extra_samples;
} wav_file_capture_config_t;

typedef struct {
  char filename[512];
  bool has_filename;
  binary_sample_format_t format;
  bool has_format;
  int channels;
  int skip_bytes;
  bool has_skip_bytes;
  int read_bytes;
  bool has_read_bytes;
  int extra_samples;
  bool has_extra_samples;
} raw_file_capture_config_t;

typedef struct {
  char filename[512];
  bool has_filename;
  binary_sample_format_t format;
  bool has_format;
  int channels;
  bool wav_header;
  bool has_wav_header;
} raw_file_playback_config_t;

typedef struct {
  int channels;
  generator_signal_t signal;
} generator_capture_config_t;

typedef struct {
  audio_backend_type_t type;
  char** labels;
  size_t labels_count;
  bool has_labels;
  bool is_wav;
  bool has_is_wav;
  union {
#if defined(ENABLE_COREAUDIO)
    coreaudio_capture_config_t coreaudio;
#endif
#if defined(ENABLE_ALSA)
    alsa_capture_config_t alsa;
#endif
#if defined(ENABLE_PULSE)
    pulse_capture_config_t pulse;
#endif
#if defined(ENABLE_PIPEWIRE)
    pipewire_capture_config_t pipewire;
#endif
#if defined(ENABLE_JACK)
    jack_capture_config_t jack;
#endif
    raw_file_capture_config_t raw_file;
    wav_file_capture_config_t wav_file;
    stdin_capture_config_t stdin_in;
    generator_capture_config_t generator;
#if defined(ENABLE_WASAPI)
    wasapi_capture_config_t wasapi;
#endif
#if defined(ENABLE_ASIO)
    asio_capture_config_t asio;
#endif
#if defined(ENABLE_BLUEZ)
    bluez_capture_config_t bluez;
#endif
  } cfg;
} capture_device_config_t;

typedef struct {
  audio_backend_type_t type;
  char** labels;
  size_t labels_count;
  bool has_labels;
  bool is_wav;
  bool has_is_wav;
  union {
#if defined(ENABLE_COREAUDIO)
    coreaudio_playback_config_t coreaudio;
#endif
#if defined(ENABLE_ALSA)
    alsa_playback_config_t alsa;
#endif
#if defined(ENABLE_PULSE)
    pulse_playback_config_t pulse;
#endif
#if defined(ENABLE_PIPEWIRE)
    pipewire_playback_config_t pipewire;
#endif
#if defined(ENABLE_JACK)
    jack_playback_config_t jack;
#endif
    raw_file_playback_config_t raw_file;
    stdout_playback_config_t stdout_out;
#if defined(ENABLE_WASAPI)
    wasapi_playback_config_t wasapi;
#endif
#if defined(ENABLE_ASIO)
    asio_playback_config_t asio;
#endif
  } cfg;
} playback_device_config_t;

typedef struct {
  size_t samplerate;
  size_t chunksize;
  bool enable_rate_adjust;
  bool has_enable_rate_adjust;
  int target_level;
  bool has_target_level;
  double adjust_period;
  bool has_adjust_period;
  resampler_config_t resampler;
  bool has_resampler;
  capture_device_config_t capture;
  playback_device_config_t playback;
  /// Capture sample rate when different from playback (requires resampler)
  size_t capture_samplerate;
  bool has_capture_samplerate;
  /// Silence detection threshold (dB). 0 = disabled.
  double silence_threshold;
  bool has_silence_threshold;
  /// Silence detection timeout (seconds). 0 = disabled.
  double silence_timeout;
  bool has_silence_timeout;
  double volume_ramp_time;
  bool has_volume_ramp_time;
  double volume_limit;
  bool has_volume_limit;
  int queuelimit;
  bool has_queuelimit;
  bool stop_on_rate_change;
  bool has_stop_on_rate_change;
  double rate_measure_interval;
  bool has_rate_measure_interval;
  bool multithreaded;
  bool has_multithreaded;
  int worker_threads;
  bool has_worker_threads;
} devices_config_t;

void capture_device_config_init(capture_device_config_t* config,
                                audio_backend_type_t type, int channels);
void playback_device_config_init(playback_device_config_t* config,
                                 audio_backend_type_t type, int channels);
void devices_config_init(devices_config_t* config, size_t samplerate,
                         size_t chunksize, capture_device_config_t capture,
                         playback_device_config_t playback);

// Accessor helper functions
int capture_device_config_get_channels(const capture_device_config_t* config);
const char* capture_device_config_get_device(
    const capture_device_config_t* config);
#if defined(ENABLE_COREAUDIO)
coreaudio_sample_format_t capture_device_config_get_format(
    const capture_device_config_t* config);
#endif
bool capture_device_config_get_bypass_dop(
    const capture_device_config_t* config);
double capture_device_config_get_dop_cutoff_hz(
    const capture_device_config_t* config);
const char* capture_device_config_get_filename(
    const capture_device_config_t* config);
binary_sample_format_t capture_device_config_get_file_format(
    const capture_device_config_t* config);
int capture_device_config_get_extra_samples(
    const capture_device_config_t* config);
int capture_device_config_get_skip_bytes(const capture_device_config_t* config);
int capture_device_config_get_read_bytes(const capture_device_config_t* config);
generator_signal_t capture_device_config_get_generator(
    const capture_device_config_t* config);

int playback_device_config_get_channels(const playback_device_config_t* config);
const char* playback_device_config_get_device(
    const playback_device_config_t* config);
#if defined(ENABLE_COREAUDIO)
coreaudio_sample_format_t playback_device_config_get_format(
    const playback_device_config_t* config);
#endif
bool playback_device_config_get_exclusive(
    const playback_device_config_t* config);
bool playback_device_config_get_output_dop(
    const playback_device_config_t* config);
sdm_filter_t playback_device_config_get_dop_encoder_filter(
    const playback_device_config_t* config);
const char* playback_device_config_get_filename(
    const playback_device_config_t* config);
binary_sample_format_t playback_device_config_get_file_format(
    const playback_device_config_t* config);
bool playback_device_config_get_wav_header(
    const playback_device_config_t* config);

void capture_device_config_set_channels(capture_device_config_t* config,
                                        int channels);
void capture_device_config_set_extra_samples(capture_device_config_t* config,
                                             int extra_samples);
#if defined(ENABLE_COREAUDIO)
void capture_device_config_set_format(capture_device_config_t* config,
                                      coreaudio_sample_format_t format);
#endif
void capture_device_config_set_file_format(capture_device_config_t* config,
                                           binary_sample_format_t format);

#endif  // CLIB_CONFIG_ENGINE_CONFIG_TYPES_H
