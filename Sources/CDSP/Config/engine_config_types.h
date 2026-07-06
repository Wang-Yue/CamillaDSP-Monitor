#ifndef CLIB_CONFIG_ENGINE_CONFIG_TYPES_H
#define CLIB_CONFIG_ENGINE_CONFIG_TYPES_H

// Standalone Engine Configuration and API Types

#include "config_error.h"
#include "resampler_config_types.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    AUDIO_BACKEND_ERR_BUFFER_EMPTY
} audio_backend_error_type_t;

typedef struct {
    audio_backend_error_type_t type;
    char message[256];
} audio_backend_error_t;

void audio_backend_error_description(const audio_backend_error_t* err, char* out_buf, size_t buf_len);

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
#if defined(__APPLE__)
typedef enum {
    COREAUDIO_SAMPLE_FORMAT_S16 = 0,
    COREAUDIO_SAMPLE_FORMAT_S24,
    COREAUDIO_SAMPLE_FORMAT_S32,
    COREAUDIO_SAMPLE_FORMAT_F32,
    COREAUDIO_SAMPLE_FORMAT_INVALID = -1
} coreaudio_sample_format_t;

const char* coreaudio_sample_format_to_string(coreaudio_sample_format_t fmt);
coreaudio_sample_format_t coreaudio_sample_format_from_string(const char* str);
#endif

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
    AUDIO_BACKEND_TYPE_CORE_AUDIO = 0,
    AUDIO_BACKEND_TYPE_ALSA = 1,
    AUDIO_BACKEND_TYPE_PULSE_AUDIO = 2,
    AUDIO_BACKEND_TYPE_PIPEWIRE = 3,
    AUDIO_BACKEND_TYPE_WASAPI = 4,
    AUDIO_BACKEND_TYPE_FILE = 5,
    AUDIO_BACKEND_TYPE_STDIN_OUT = 6,
    AUDIO_BACKEND_TYPE_GENERATOR = 7,
    AUDIO_BACKEND_TYPE_ASIO = 8,
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

#if defined(__linux__)
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

#if defined(_WIN32)
typedef enum {
    WASAPI_SAMPLE_FORMAT_S16 = 0,
    WASAPI_SAMPLE_FORMAT_S24,
    WASAPI_SAMPLE_FORMAT_S32,
    WASAPI_SAMPLE_FORMAT_F32,
    WASAPI_SAMPLE_FORMAT_INVALID = -1
} wasapi_sample_format_t;

const char* wasapi_sample_format_to_string(wasapi_sample_format_t fmt);
wasapi_sample_format_t wasapi_sample_format_from_string(const char* str);

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

typedef struct {
    audio_backend_type_t type;
    int channels;
    char device[256];
    bool has_device;
#if defined(__linux__)
    alsa_sample_format_t format;
#elif defined(__APPLE__)
    coreaudio_sample_format_t format;
#elif defined(_WIN32)
    wasapi_sample_format_t format;
    asio_sample_format_t asio_format;
    bool has_asio_format;
#endif
    bool has_format;
    bool loopback;
    bool has_loopback;
#if defined(__linux__)
    bool stop_on_inactive;
    bool has_stop_on_inactive;
    char link_volume_control[256];
    bool has_link_volume_control;
    char link_mute_control[256];
    bool has_link_mute_control;
#endif
    /// If true, bypass DoP detection and handle signal strictly as PCM. Default is false.
    bool bypass_dop;
    bool has_bypass_dop;
    /// DoP decimator passband cutoff in Hz. Lower values give higher SINAD by
    /// rejecting more DSD shaping noise; higher values widen the audible
    /// passband (and let through more ultrasonic content). Default 20 kHz.
    double dop_cutoff_hz;
    bool has_dop_cutoff_hz;
    generator_signal_t generator;
    bool has_generator;
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
} capture_device_config_t;

typedef struct {
    audio_backend_type_t type;
    int channels;
    char device[256];
    bool has_device;
#if defined(__linux__)
    alsa_sample_format_t format;
#elif defined(__APPLE__)
    coreaudio_sample_format_t format;
#elif defined(_WIN32)
    wasapi_sample_format_t format;
    asio_sample_format_t asio_format;
    bool has_asio_format;
#endif
    bool has_format;
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
} playback_device_config_t;

typedef struct {
    int samplerate;
    int chunksize;
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
    int capture_samplerate;
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

void capture_device_config_init(capture_device_config_t* config, audio_backend_type_t type, int channels);
void playback_device_config_init(playback_device_config_t* config, audio_backend_type_t type, int channels);
void devices_config_init(devices_config_t* config, int samplerate, int chunksize, capture_device_config_t capture, playback_device_config_t playback);

#ifdef __cplusplus
}
#endif

#endif // CLIB_CONFIG_ENGINE_CONFIG_TYPES_H
