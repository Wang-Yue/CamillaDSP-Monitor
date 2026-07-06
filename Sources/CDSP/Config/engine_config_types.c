#include "engine_config_types.h"
#include <string.h>
#include <stdio.h>
#include <strings.h>

// Standalone Engine Configuration and API Types

/// Engine processing state.
uint8_t processing_state_to_raw_byte(processing_state_t state) {
    switch (state) {
        case PROCESSING_STATE_INACTIVE: return 0;
        case PROCESSING_STATE_STARTING: return 1;
        case PROCESSING_STATE_RUNNING: return 2;
        case PROCESSING_STATE_PAUSED: return 3;
        case PROCESSING_STATE_STALLED: return 4;
        default: return 0;
    }
}

processing_state_t processing_state_from_raw_byte(uint8_t raw_byte) {
    switch (raw_byte) {
        case 1: return PROCESSING_STATE_STARTING;
        case 2: return PROCESSING_STATE_RUNNING;
        case 3: return PROCESSING_STATE_PAUSED;
        case 4: return PROCESSING_STATE_STALLED;
        default: return PROCESSING_STATE_INACTIVE;
    }
}

const char* processing_state_to_string(processing_state_t state) {
    switch (state) {
        case PROCESSING_STATE_INACTIVE: return "Inactive";
        case PROCESSING_STATE_STARTING: return "Starting";
        case PROCESSING_STATE_RUNNING: return "Running";
        case PROCESSING_STATE_PAUSED: return "Paused";
        case PROCESSING_STATE_STALLED: return "Stalled";
        default: return "Inactive";
    }
}

processing_state_t processing_state_from_string(const char* str) {
    if (!str) return PROCESSING_STATE_INACTIVE;
    if (strcmp(str, "Starting") == 0) return PROCESSING_STATE_STARTING;
    if (strcmp(str, "Running") == 0) return PROCESSING_STATE_RUNNING;
    if (strcmp(str, "Paused") == 0) return PROCESSING_STATE_PAUSED;
    if (strcmp(str, "Stalled") == 0) return PROCESSING_STATE_STALLED;
    return PROCESSING_STATE_INACTIVE;
}

void audio_backend_error_description(const audio_backend_error_t* err, char* out_buf, size_t buf_len) {
    if (!err || !out_buf || buf_len == 0) return;
    switch (err->type) {
        case AUDIO_BACKEND_ERR_CONFIG_PARSE:
            snprintf(out_buf, buf_len, "Config parse error: %s", err->message);
            break;
        case AUDIO_BACKEND_ERR_COMMAND_SEND:
            snprintf(out_buf, buf_len, "Command send error: %s", err->message);
            break;
        case AUDIO_BACKEND_ERR_INVALID_SAMPLERATE:
            snprintf(out_buf, buf_len, "Invalid samplerate: %s", err->message);
            break;
        case AUDIO_BACKEND_ERR_SPECTRUM_COMPUTE:
            snprintf(out_buf, buf_len, "Spectrum compute error: %s", err->message);
            break;
        case AUDIO_BACKEND_ERR_ENGINE_NOT_RUNNING:
            snprintf(out_buf, buf_len, "Engine not running");
            break;
        case AUDIO_BACKEND_ERR_BUFFER_EMPTY:
            snprintf(out_buf, buf_len, "Audio history buffer is empty");
            break;
        default:
            out_buf[0] = '\0';
            break;
    }
}

// MARK: - Capability data model
#if defined(__APPLE__)
const char* coreaudio_sample_format_to_string(coreaudio_sample_format_t fmt) {
    switch (fmt) {
        case COREAUDIO_SAMPLE_FORMAT_S16: return "S16";
        case COREAUDIO_SAMPLE_FORMAT_S24: return "S24";
        case COREAUDIO_SAMPLE_FORMAT_S32: return "S32";
        case COREAUDIO_SAMPLE_FORMAT_F32: return "F32";
        default: return "Invalid";
    }
}

coreaudio_sample_format_t coreaudio_sample_format_from_string(const char* str) {
    if (!str) return COREAUDIO_SAMPLE_FORMAT_INVALID;
    if (strcmp(str, "S16") == 0) return COREAUDIO_SAMPLE_FORMAT_S16;
    if (strcmp(str, "S24") == 0) return COREAUDIO_SAMPLE_FORMAT_S24;
    if (strcmp(str, "S32") == 0) return COREAUDIO_SAMPLE_FORMAT_S32;
    if (strcmp(str, "F32") == 0) return COREAUDIO_SAMPLE_FORMAT_F32;
    return COREAUDIO_SAMPLE_FORMAT_INVALID;
}
#endif

#if defined(__linux__)
const char* alsa_sample_format_to_string(alsa_sample_format_t fmt) {
    switch (fmt) {
        case ALSA_SAMPLE_FORMAT_S16_LE: return "S16_LE";
        case ALSA_SAMPLE_FORMAT_S24_3_LE: return "S24_3_LE";
        case ALSA_SAMPLE_FORMAT_S24_4_LE: return "S24_4_LE";
        case ALSA_SAMPLE_FORMAT_S32_LE: return "S32_LE";
        case ALSA_SAMPLE_FORMAT_F32_LE: return "F32_LE";
        case ALSA_SAMPLE_FORMAT_F64_LE: return "F64_LE";
        default: return "Invalid";
    }
}

alsa_sample_format_t alsa_sample_format_from_string(const char* str) {
    if (!str) return ALSA_SAMPLE_FORMAT_INVALID;
    if (strcmp(str, "S16_LE") == 0) return ALSA_SAMPLE_FORMAT_S16_LE;
    if (strcmp(str, "S24_3_LE") == 0) return ALSA_SAMPLE_FORMAT_S24_3_LE;
    if (strcmp(str, "S24_4_LE") == 0) return ALSA_SAMPLE_FORMAT_S24_4_LE;
    if (strcmp(str, "S32_LE") == 0) return ALSA_SAMPLE_FORMAT_S32_LE;
    if (strcmp(str, "F32_LE") == 0) return ALSA_SAMPLE_FORMAT_F32_LE;
    if (strcmp(str, "F64_LE") == 0) return ALSA_SAMPLE_FORMAT_F64_LE;
    return ALSA_SAMPLE_FORMAT_INVALID;
}
#endif

// MARK: - Device Config Models

const char* audio_backend_type_to_string(audio_backend_type_t type) {
    switch (type) {
#if defined(__APPLE__)
        case AUDIO_BACKEND_TYPE_CORE_AUDIO: return "CoreAudio";
#elif defined(__linux__)
        case AUDIO_BACKEND_TYPE_ALSA: return "Alsa";
        case AUDIO_BACKEND_TYPE_PULSE_AUDIO: return "Pulse";
        case AUDIO_BACKEND_TYPE_PIPEWIRE: return "Pipewire";
#elif defined(_WIN32)
        case AUDIO_BACKEND_TYPE_WASAPI: return "Wasapi";
        case AUDIO_BACKEND_TYPE_ASIO: return "Asio";
#endif
        case AUDIO_BACKEND_TYPE_FILE: return "File";
        case AUDIO_BACKEND_TYPE_STDIN_OUT: return "Stdin";
        case AUDIO_BACKEND_TYPE_GENERATOR: return "SignalGenerator";
        default: return "Unknown";
    }
}

audio_backend_type_t audio_backend_type_from_string(const char* str) {
    if (!str) return AUDIO_BACKEND_TYPE_INVALID;
#if defined(__APPLE__)
    if (strcasecmp(str, "CoreAudio") == 0 || strcasecmp(str, "Core Audio") == 0) return AUDIO_BACKEND_TYPE_CORE_AUDIO;
#elif defined(__linux__)
    if (strcasecmp(str, "Alsa") == 0 || strcasecmp(str, "ALSA") == 0) return AUDIO_BACKEND_TYPE_ALSA;
    if (strcasecmp(str, "Pulse") == 0 || strcasecmp(str, "PulseAudio") == 0) return AUDIO_BACKEND_TYPE_PULSE_AUDIO;
    if (strcasecmp(str, "Pipewire") == 0 || strcasecmp(str, "PipeWire") == 0) return AUDIO_BACKEND_TYPE_PIPEWIRE;
#elif defined(_WIN32)
    if (strcasecmp(str, "Wasapi") == 0 || strcasecmp(str, "WASAPI") == 0) return AUDIO_BACKEND_TYPE_WASAPI;
    if (strcasecmp(str, "Asio") == 0 || strcasecmp(str, "ASIO") == 0) return AUDIO_BACKEND_TYPE_ASIO;
#endif
    if (strcasecmp(str, "File") == 0 || strcasecmp(str, "RawFile") == 0 || strcasecmp(str, "WavFile") == 0) return AUDIO_BACKEND_TYPE_FILE;
    if (strcasecmp(str, "Stdin") == 0 || strcasecmp(str, "Stdout") == 0 || strcasecmp(str, "STDIN") == 0 || strcasecmp(str, "STDOUT") == 0) return AUDIO_BACKEND_TYPE_STDIN_OUT;
    if (strcasecmp(str, "SignalGenerator") == 0 || strcasecmp(str, "Generator") == 0) return AUDIO_BACKEND_TYPE_GENERATOR;
    return AUDIO_BACKEND_TYPE_INVALID;
}

const char* signal_type_to_string(signal_type_t type) {
    switch (type) {
        case SIGNAL_TYPE_SINE: return "Sine";
        case SIGNAL_TYPE_SQUARE: return "Square";
        case SIGNAL_TYPE_WHITE_NOISE: return "WhiteNoise";
        default: return "Invalid";
    }
}

signal_type_t signal_type_from_string(const char* str) {
    if (!str) return SIGNAL_TYPE_INVALID;
    if (strcasecmp(str, "Sine") == 0) return SIGNAL_TYPE_SINE;
    if (strcasecmp(str, "Square") == 0) return SIGNAL_TYPE_SQUARE;
    if (strcasecmp(str, "WhiteNoise") == 0 || strcasecmp(str, "White Noise") == 0) return SIGNAL_TYPE_WHITE_NOISE;
    return SIGNAL_TYPE_INVALID;
}

const char* sdm_filter_to_string(sdm_filter_t filter) {
    switch (filter) {
        case SDM_FILTER_CLANS4: return "clans-4";
        case SDM_FILTER_SDM4: return "sdm-4";
        case SDM_FILTER_CLANS5: return "clans-5";
        case SDM_FILTER_SDM5: return "sdm-5";
        case SDM_FILTER_CLANS6: return "clans-6";
        case SDM_FILTER_SDM6: return "sdm-6";
        case SDM_FILTER_CLANS7: return "clans-7";
        case SDM_FILTER_SDM7: return "sdm-7";
        case SDM_FILTER_CLANS8: return "clans-8";
        case SDM_FILTER_SDM8: return "sdm-8";
        default: return "sdm-6";
    }
}

sdm_filter_t sdm_filter_from_string(const char* str) {
    if (!str) return SDM_FILTER_INVALID;
    if (strcmp(str, "clans-4") == 0) return SDM_FILTER_CLANS4;
    if (strcmp(str, "sdm-4") == 0) return SDM_FILTER_SDM4;
    if (strcmp(str, "clans-5") == 0) return SDM_FILTER_CLANS5;
    if (strcmp(str, "sdm-5") == 0) return SDM_FILTER_SDM5;
    if (strcmp(str, "clans-6") == 0) return SDM_FILTER_CLANS6;
    if (strcmp(str, "sdm-6") == 0) return SDM_FILTER_SDM6;
    if (strcmp(str, "clans-7") == 0) return SDM_FILTER_CLANS7;
    if (strcmp(str, "sdm-7") == 0) return SDM_FILTER_SDM7;
    if (strcmp(str, "clans-8") == 0) return SDM_FILTER_CLANS8;
    if (strcmp(str, "sdm-8") == 0) return SDM_FILTER_SDM8;
    return SDM_FILTER_INVALID;
}

const char* binary_sample_format_to_string(binary_sample_format_t fmt) {
    switch (fmt) {
        case BINARY_SAMPLE_FORMAT_S16_LE: return "S16_LE";
        case BINARY_SAMPLE_FORMAT_S24_3_LE: return "S24_3_LE";
        case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE: return "S24_4_RJ_LE";
        case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE: return "S24_4_LJ_LE";
        case BINARY_SAMPLE_FORMAT_S32_LE: return "S32_LE";
        case BINARY_SAMPLE_FORMAT_F32_LE: return "F32_LE";
        case BINARY_SAMPLE_FORMAT_F64_LE: return "F64_LE";
        default: return "Invalid";
    }
}

binary_sample_format_t binary_sample_format_from_string(const char* str) {
    if (!str) return BINARY_SAMPLE_FORMAT_INVALID;
    if (strcmp(str, "S16_LE") == 0) return BINARY_SAMPLE_FORMAT_S16_LE;
    if (strcmp(str, "S24_3_LE") == 0) return BINARY_SAMPLE_FORMAT_S24_3_LE;
    if (strcmp(str, "S24_4_RJ_LE") == 0) return BINARY_SAMPLE_FORMAT_S24_4_RJ_LE;
    if (strcmp(str, "S24_4_LJ_LE") == 0) return BINARY_SAMPLE_FORMAT_S24_4_LJ_LE;
    if (strcmp(str, "S32_LE") == 0) return BINARY_SAMPLE_FORMAT_S32_LE;
    if (strcmp(str, "F32_LE") == 0) return BINARY_SAMPLE_FORMAT_F32_LE;
    if (strcmp(str, "F64_LE") == 0) return BINARY_SAMPLE_FORMAT_F64_LE;
    return BINARY_SAMPLE_FORMAT_INVALID;
}

#if defined(_WIN32)
const char* wasapi_sample_format_to_string(wasapi_sample_format_t fmt) {
    switch (fmt) {
        case WASAPI_SAMPLE_FORMAT_S16: return "S16";
        case WASAPI_SAMPLE_FORMAT_S24: return "S24";
        case WASAPI_SAMPLE_FORMAT_S32: return "S32";
        case WASAPI_SAMPLE_FORMAT_F32: return "F32";
        default: return "Invalid";
    }
}

wasapi_sample_format_t wasapi_sample_format_from_string(const char* str) {
    if (!str) return WASAPI_SAMPLE_FORMAT_INVALID;
    if (strcmp(str, "S16") == 0) return WASAPI_SAMPLE_FORMAT_S16;
    if (strcmp(str, "S24") == 0) return WASAPI_SAMPLE_FORMAT_S24;
    if (strcmp(str, "S32") == 0) return WASAPI_SAMPLE_FORMAT_S32;
    if (strcmp(str, "F32") == 0) return WASAPI_SAMPLE_FORMAT_F32;
    return WASAPI_SAMPLE_FORMAT_INVALID;
}

const char* asio_sample_format_to_string(asio_sample_format_t fmt) {
    switch (fmt) {
        case ASIO_SAMPLE_FORMAT_S16_LE: return "S16_LE";
        case ASIO_SAMPLE_FORMAT_S24_3_LE: return "S24_3_LE";
        case ASIO_SAMPLE_FORMAT_S24_4_LE: return "S24_4_LE";
        case ASIO_SAMPLE_FORMAT_S32_LE: return "S32_LE";
        case ASIO_SAMPLE_FORMAT_F32_LE: return "F32_LE";
        case ASIO_SAMPLE_FORMAT_F64_LE: return "F64_LE";
        default: return "Invalid";
    }
}

asio_sample_format_t asio_sample_format_from_string(const char* str) {
    if (!str) return ASIO_SAMPLE_FORMAT_INVALID;
    if (strcmp(str, "S16_LE") == 0) return ASIO_SAMPLE_FORMAT_S16_LE;
    if (strcmp(str, "S24_3_LE") == 0) return ASIO_SAMPLE_FORMAT_S24_3_LE;
    if (strcmp(str, "S24_4_LE") == 0) return ASIO_SAMPLE_FORMAT_S24_4_LE;
    if (strcmp(str, "S32_LE") == 0) return ASIO_SAMPLE_FORMAT_S32_LE;
    if (strcmp(str, "F32_LE") == 0) return ASIO_SAMPLE_FORMAT_F32_LE;
    if (strcmp(str, "F64_LE") == 0) return ASIO_SAMPLE_FORMAT_F64_LE;
    return ASIO_SAMPLE_FORMAT_INVALID;
}
#endif

/// If true, bypass DoP detection and handle signal strictly as PCM. Default is false.
/// DoP decimator passband cutoff in Hz. Lower values give higher SINAD by
/// rejecting more DSD shaping noise; higher values widen the audible
/// passband (and let through more ultrasonic content). Default 20 kHz.
void capture_device_config_init(capture_device_config_t* config, audio_backend_type_t type, int channels) {
    if (!config) return;
    memset(config, 0, sizeof(capture_device_config_t));
    config->type = type;
    config->channels = channels;
}

void playback_device_config_init(playback_device_config_t* config, audio_backend_type_t type, int channels) {
    if (!config) return;
    memset(config, 0, sizeof(playback_device_config_t));
    config->type = type;
    config->channels = channels;
}

/// Capture sample rate when different from playback (requires resampler)
/// Silence detection threshold (dB). 0 = disabled.
/// Silence detection timeout (seconds). 0 = disabled.
void devices_config_init(devices_config_t* config, size_t samplerate, size_t chunksize, capture_device_config_t capture, playback_device_config_t playback) {
    if (!config) return;
    memset(config, 0, sizeof(devices_config_t));
    config->samplerate = samplerate;
    config->chunksize = chunksize;
    config->capture = capture;
    config->playback = playback;
}
