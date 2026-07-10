/**
 * @file engine_config_types.h
 * @brief Standalone Engine Configuration and API Types.
 */

#ifndef CLIB_CONFIG_ENGINE_CONFIG_TYPES_H
#define CLIB_CONFIG_ENGINE_CONFIG_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config_error.h"
#include "resampler_config_types.h"

/**
 * @brief Engine processing state.
 */
typedef enum {
  PROCESSING_STATE_INACTIVE = 0, /**< Engine is inactive. */
  PROCESSING_STATE_STARTING = 1, /**< Engine is starting. */
  PROCESSING_STATE_RUNNING = 2,  /**< Engine is running. */
  PROCESSING_STATE_PAUSED = 3,   /**< Engine is paused. */
  PROCESSING_STATE_STALLED =
      4 /**< Engine is stalled (e.g., waiting for data). */
} processing_state_t;

/**
 * @brief Converts processing state to a raw byte for transmission/storage.
 * @param state The processing state.
 * @return Raw byte representation.
 */
uint8_t processing_state_to_raw_byte(processing_state_t state);

/**
 * @brief Converts a raw byte back to processing state.
 * @param raw_byte The raw byte.
 * @return The processing state.
 */
processing_state_t processing_state_from_raw_byte(uint8_t raw_byte);

/**
 * @brief Converts processing state to string.
 * @param state The processing state.
 * @return String representation.
 */
const char* processing_state_to_string(processing_state_t state);

/**
 * @brief Parses processing state from string.
 * @param str The string representation.
 * @return The processing state.
 */
processing_state_t processing_state_from_string(const char* str);

/**
 * @brief Reason why the engine stopped.
 */
typedef enum {
  STOP_REASON_NONE = 0,               /**< Not stopped. */
  STOP_REASON_DONE,                   /**< Finished processing (e.g., EOF). */
  STOP_REASON_CAPTURE_ERROR,          /**< Error in capture device. */
  STOP_REASON_PLAYBACK_ERROR,         /**< Error in playback device. */
  STOP_REASON_CAPTURE_FORMAT_CHANGE,  /**< Capture format changed. */
  STOP_REASON_PLAYBACK_FORMAT_CHANGE, /**< Playback format changed. */
  STOP_REASON_UNKNOWN_ERROR           /**< Unknown error. */
} processing_stop_reason_type_t;

/**
 * @brief Structure containing detailed stop reason.
 */
typedef struct {
  processing_stop_reason_type_t type; /**< Type of stop reason. */
  char message[256];                  /**< Detailed error message. */
  int format_change_rate;             /**< New sample rate if format changed. */
} processing_stop_reason_t;

/**
 * @brief State update structure.
 */
typedef struct {
  processing_state_t state; /**< Current processing state. */
  processing_stop_reason_t
      stop_reason; /**< Stop reason (if inactive/stopped). */
} state_update_t;

/**
 * @brief Representation of an audio device.
 */
typedef struct {
  char name[256]; /**< Device name. */
} audio_device_t;

/**
 * @brief Audio backend error types.
 */
typedef enum {
  AUDIO_BACKEND_ERR_CONFIG_PARSE = 0, /**< Configuration parsing error. */
  AUDIO_BACKEND_ERR_COMMAND_SEND,     /**< Error sending command to backend. */
  AUDIO_BACKEND_ERR_INVALID_SAMPLERATE, /**< Invalid sample rate. */
  AUDIO_BACKEND_ERR_SPECTRUM_COMPUTE,   /**< Error computing spectrum. */
  AUDIO_BACKEND_ERR_ENGINE_NOT_RUNNING, /**< Engine is not running. */
  AUDIO_BACKEND_ERR_BUFFER_EMPTY,       /**< Buffer is empty. */
  AUDIO_BACKEND_ERR_DEVICE_NOT_FOUND,   /**< Audio device not found. */
  AUDIO_BACKEND_ERR_DEVICE_BUSY         /**< Audio device is busy. */
} audio_backend_error_type_t;

/**
 * @brief Audio backend error structure.
 */
typedef struct {
  audio_backend_error_type_t type; /**< Error type. */
  char message[256];               /**< Error message. */
} audio_backend_error_t;

/**
 * @brief Gets description of audio backend error.
 * @param err The error.
 * @param out_buf Output buffer.
 * @param buf_len Output buffer length.
 */
void audio_backend_error_description(const audio_backend_error_t* err,
                                     char* out_buf, size_t buf_len);

/**
 * @brief VU levels for playback and capture.
 */
typedef struct {
  double* playback_rms;     /**< Array of playback RMS levels per channel. */
  double* playback_peak;    /**< Array of playback peak levels per channel. */
  double* capture_rms;      /**< Array of capture RMS levels per channel. */
  double* capture_peak;     /**< Array of capture peak levels per channel. */
  size_t playback_channels; /**< Number of playback channels. */
  size_t capture_channels;  /**< Number of capture channels. */
} vu_levels_t;

/**
 * @brief Frequency spectrum data.
 */
typedef struct {
  double* frequencies; /**< Array of frequencies. */
  double* magnitudes;  /**< Array of magnitudes. */
  size_t count;        /**< Number of points. */
} spectrum_t;

/**
 * @brief Audio samples buffer.
 */
typedef struct {
  double** channels;     /**< Array of channel buffers. */
  size_t channels_count; /**< Number of channels. */
  size_t frames;         /**< Number of frames per channel. */
} audio_samples_t;

// MARK: - Capability data model
#if defined(ENABLE_COREAUDIO)
/**
 * @brief CoreAudio sample formats.
 */
typedef enum {
  COREAUDIO_SAMPLE_FORMAT_S16 = 0,     /**< Signed 16-bit integer. */
  COREAUDIO_SAMPLE_FORMAT_S24,         /**< Signed 24-bit integer. */
  COREAUDIO_SAMPLE_FORMAT_S32,         /**< Signed 32-bit integer. */
  COREAUDIO_SAMPLE_FORMAT_F32,         /**< 32-bit float. */
  COREAUDIO_SAMPLE_FORMAT_INVALID = -1 /**< Invalid format. */
} coreaudio_sample_format_t;

/**
 * @brief Converts CoreAudio sample format to string.
 * @param fmt The format.
 * @return String representation.
 */
const char* coreaudio_sample_format_to_string(coreaudio_sample_format_t fmt);

/**
 * @brief Parses CoreAudio sample format from string.
 * @param str The string representation.
 * @return The format.
 */
coreaudio_sample_format_t coreaudio_sample_format_from_string(const char* str);
#endif  // ENABLE_COREAUDIO

/**
 * @brief Sample rate capabilities.
 */
typedef struct {
  int samplerate;       /**< Supported sample rate. */
  char** formats;       /**< Supported formats at this sample rate. */
  size_t formats_count; /**< Number of formats. */
} samplerate_capability_t;

/**
 * @brief Channel capabilities.
 */
typedef struct {
  int channels; /**< Supported number of channels. */
  samplerate_capability_t*
      samplerates; /**< Supported sample rates for this channel count. */
  size_t samplerates_count; /**< Number of sample rates. */
} channel_capability_t;

/**
 * @brief Device capability set.
 */
typedef struct {
  channel_capability_t* capabilities; /**< Array of channel capabilities. */
  size_t capabilities_count;          /**< Number of channel capabilities. */
} device_capability_set_t;

/**
 * @brief Detailed description of an audio device and its capabilities.
 */
typedef struct {
  char name[256];                           /**< Device name. */
  device_capability_set_t* capability_sets; /**< Array of capability sets. */
  size_t capability_sets_count;             /**< Number of capability sets. */
} audio_device_descriptor_t;

// MARK: - Device Config Models

/**
 * @brief Audio I/O backend types.
 */
typedef enum {
#if defined(ENABLE_COREAUDIO)
  AUDIO_BACKEND_TYPE_CORE_AUDIO = 0, /**< CoreAudio (macOS). */
#endif
#if defined(ENABLE_ALSA)
  AUDIO_BACKEND_TYPE_ALSA = 1, /**< ALSA (Linux). */
#endif
#if defined(ENABLE_PULSE)
  AUDIO_BACKEND_TYPE_PULSE_AUDIO = 2, /**< PulseAudio. */
#endif
#if defined(ENABLE_PIPEWIRE)
  AUDIO_BACKEND_TYPE_PIPEWIRE = 3, /**< PipeWire. */
#endif
#if defined(ENABLE_WASAPI)
  AUDIO_BACKEND_TYPE_WASAPI = 4, /**< WASAPI (Windows). */
#endif
#if defined(ENABLE_ASIO)
  AUDIO_BACKEND_TYPE_ASIO = 8, /**< ASIO (Windows). */
#endif
#if defined(ENABLE_JACK)
  AUDIO_BACKEND_TYPE_JACK = 9, /**< JACK. */
#endif
#if defined(ENABLE_BLUEZ)
  AUDIO_BACKEND_TYPE_BLUEZ = 10, /**< Bluez (Bluetooth). */
#endif
  AUDIO_BACKEND_TYPE_FILE = 5,      /**< File input/output. */
  AUDIO_BACKEND_TYPE_STDIN_OUT = 6, /**< Standard input/output. */
  AUDIO_BACKEND_TYPE_GENERATOR = 7, /**< Signal generator. */
  AUDIO_BACKEND_TYPE_INVALID = -1   /**< Invalid backend. */
} audio_backend_type_t;

/**
 * @brief Converts audio backend type to string.
 * @param type The backend type.
 * @return String representation.
 */
const char* audio_backend_type_to_string(audio_backend_type_t type);

/**
 * @brief Parses audio backend type from string.
 * @param str The string representation.
 * @return The backend type.
 */
audio_backend_type_t audio_backend_type_from_string(const char* str);

/**
 * @brief Sigma-Delta Modulator (SDM) filter types for DoP (DSD over PCM).
 */
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

/**
 * @brief Converts SDM filter type to string.
 * @param filter The filter type.
 * @return String representation.
 */
const char* sdm_filter_to_string(sdm_filter_t filter);

/**
 * @brief Parses SDM filter type from string.
 * @param str The string representation.
 * @return The filter type.
 */
sdm_filter_t sdm_filter_from_string(const char* str);

#if defined(ENABLE_ALSA)
/**
 * @brief ALSA sample formats.
 */
typedef enum {
  ALSA_SAMPLE_FORMAT_S16_LE = 0,  /**< Signed 16-bit Little Endian. */
  ALSA_SAMPLE_FORMAT_S24_3_LE,    /**< Signed 24-bit Little Endian (3 bytes). */
  ALSA_SAMPLE_FORMAT_S24_4_LE,    /**< Signed 24-bit Little Endian (4 bytes). */
  ALSA_SAMPLE_FORMAT_S32_LE,      /**< Signed 32-bit Little Endian. */
  ALSA_SAMPLE_FORMAT_F32_LE,      /**< 32-bit Float Little Endian. */
  ALSA_SAMPLE_FORMAT_F64_LE,      /**< 64-bit Float Little Endian. */
  ALSA_SAMPLE_FORMAT_INVALID = -1 /**< Invalid format. */
} alsa_sample_format_t;

/**
 * @brief Converts ALSA sample format to string.
 * @param fmt The format.
 * @return String representation.
 */
const char* alsa_sample_format_to_string(alsa_sample_format_t fmt);

/**
 * @brief Parses ALSA sample format from string.
 * @param str The string representation.
 * @return The format.
 */
alsa_sample_format_t alsa_sample_format_from_string(const char* str);
#endif

#if defined(ENABLE_WASAPI)
/**
 * @brief WASAPI sample formats.
 */
typedef enum {
  WASAPI_SAMPLE_FORMAT_S16 = 0,     /**< Signed 16-bit integer. */
  WASAPI_SAMPLE_FORMAT_S24,         /**< Signed 24-bit integer. */
  WASAPI_SAMPLE_FORMAT_S32,         /**< Signed 32-bit integer. */
  WASAPI_SAMPLE_FORMAT_F32,         /**< 32-bit float. */
  WASAPI_SAMPLE_FORMAT_INVALID = -1 /**< Invalid format. */
} wasapi_sample_format_t;

/**
 * @brief Converts WASAPI sample format to string.
 * @param fmt The format.
 * @return String representation.
 */
const char* wasapi_sample_format_to_string(wasapi_sample_format_t fmt);

/**
 * @brief Parses WASAPI sample format from string.
 * @param str The string representation.
 * @return The format.
 */
wasapi_sample_format_t wasapi_sample_format_from_string(const char* str);
#endif

#if defined(ENABLE_ASIO)
/**
 * @brief ASIO sample formats.
 */
typedef enum {
  ASIO_SAMPLE_FORMAT_S16_LE = 0,  /**< Signed 16-bit Little Endian. */
  ASIO_SAMPLE_FORMAT_S24_3_LE,    /**< Signed 24-bit Little Endian (3 bytes). */
  ASIO_SAMPLE_FORMAT_S24_4_LE,    /**< Signed 24-bit Little Endian (4 bytes). */
  ASIO_SAMPLE_FORMAT_S32_LE,      /**< Signed 32-bit Little Endian. */
  ASIO_SAMPLE_FORMAT_F32_LE,      /**< 32-bit Float Little Endian. */
  ASIO_SAMPLE_FORMAT_F64_LE,      /**< 64-bit Float Little Endian. */
  ASIO_SAMPLE_FORMAT_INVALID = -1 /**< Invalid format. */
} asio_sample_format_t;

/**
 * @brief Converts ASIO sample format to string.
 * @param fmt The format.
 * @return String representation.
 */
const char* asio_sample_format_to_string(asio_sample_format_t fmt);

/**
 * @brief Parses ASIO sample format from string.
 * @param str The string representation.
 * @return The format.
 */
asio_sample_format_t asio_sample_format_from_string(const char* str);
#endif

/**
 * @brief Binary (raw file/stream) sample formats.
 */
typedef enum {
  BINARY_SAMPLE_FORMAT_S16_LE = 0, /**< Signed 16-bit Little Endian. */
  BINARY_SAMPLE_FORMAT_S24_3_LE, /**< Signed 24-bit Little Endian (3 bytes). */
  BINARY_SAMPLE_FORMAT_S24_4_RJ_LE, /**< Signed 24-bit Right Justified in 4
                                       bytes Little Endian. */
  BINARY_SAMPLE_FORMAT_S24_4_LJ_LE, /**< Signed 24-bit Left Justified in 4 bytes
                                       Little Endian. */
  BINARY_SAMPLE_FORMAT_S32_LE,      /**< Signed 32-bit Little Endian. */
  BINARY_SAMPLE_FORMAT_F32_LE,      /**< 32-bit Float Little Endian. */
  BINARY_SAMPLE_FORMAT_F64_LE,      /**< 64-bit Float Little Endian. */
  BINARY_SAMPLE_FORMAT_INVALID = -1 /**< Invalid format. */
} binary_sample_format_t;

/**
 * @brief Converts binary sample format to string.
 * @param fmt The format.
 * @return String representation.
 */
const char* binary_sample_format_to_string(binary_sample_format_t fmt);

/**
 * @brief Parses binary sample format from string.
 * @param str The string representation.
 * @return The format.
 */
binary_sample_format_t binary_sample_format_from_string(const char* str);

/**
 * @brief Signal generator types.
 */
typedef enum {
  SIGNAL_TYPE_SINE = 0,    /**< Sine wave. */
  SIGNAL_TYPE_SQUARE,      /**< Square wave. */
  SIGNAL_TYPE_WHITE_NOISE, /**< White noise. */
  SIGNAL_TYPE_INVALID = -1 /**< Invalid signal type. */
} signal_type_t;

/**
 * @brief Converts signal type to string.
 * @param type The signal type.
 * @return String representation.
 */
const char* signal_type_to_string(signal_type_t type);

/**
 * @brief Parses signal type from string.
 * @param str The string representation.
 * @return The signal type.
 */
signal_type_t signal_type_from_string(const char* str);

/**
 * @brief Signal generator parameters.
 */
typedef struct {
  signal_type_t type; /**< Signal type. */
  double frequency;   /**< Frequency in Hz. */
  double level;       /**< Signal level. */
} generator_signal_t;

#if defined(ENABLE_COREAUDIO)
/**
 * @brief CoreAudio capture configuration.
 */
typedef struct {
  int channels;                     /**< Number of channels. */
  char device[256];                 /**< Device name. */
  bool has_device;                  /**< True if custom device is specified. */
  coreaudio_sample_format_t format; /**< Sample format. */
  bool has_format;                  /**< True if format is specified. */
} coreaudio_capture_config_t;

/**
 * @brief CoreAudio playback configuration.
 */
typedef struct {
  int channels;                     /**< Number of channels. */
  char device[256];                 /**< Device name. */
  bool has_device;                  /**< True if custom device is specified. */
  coreaudio_sample_format_t format; /**< Sample format. */
  bool has_format;                  /**< True if format is specified. */
  bool exclusive;                   /**< Use exclusive mode. */
  bool has_exclusive;               /**< True if exclusive is specified. */
} coreaudio_playback_config_t;
#endif

#if defined(ENABLE_ALSA)
/**
 * @brief ALSA capture configuration.
 */
typedef struct {
  int channels;                  /**< Number of channels. */
  char device[256];              /**< Device name. */
  alsa_sample_format_t format;   /**< Sample format. */
  bool has_format;               /**< True if format is specified. */
  bool stop_on_inactive;         /**< Stop when inactive. */
  bool has_stop_on_inactive;     /**< True if stop_on_inactive is specified. */
  char link_volume_control[256]; /**< Name of ALSA control to link volume to. */
  bool
      has_link_volume_control; /**< True if link_volume_control is specified. */
  char link_mute_control[256]; /**< Name of ALSA control to link mute to. */
  bool has_link_mute_control;  /**< True if link_mute_control is specified. */
} alsa_capture_config_t;

/**
 * @brief ALSA playback configuration.
 */
typedef struct {
  int channels;                /**< Number of channels. */
  char device[256];            /**< Device name. */
  alsa_sample_format_t format; /**< Sample format. */
  bool has_format;             /**< True if format is specified. */
} alsa_playback_config_t;
#endif

#if defined(ENABLE_PULSE)
/**
 * @brief PulseAudio capture configuration.
 */
typedef struct {
  int channels;     /**< Number of channels. */
  char device[256]; /**< Device/source name. */
} pulse_capture_config_t;

/**
 * @brief PulseAudio playback configuration.
 */
typedef struct {
  int channels;     /**< Number of channels. */
  char device[256]; /**< Device/sink name. */
} pulse_playback_config_t;
#endif

#if defined(ENABLE_PIPEWIRE)
/**
 * @brief PipeWire capture configuration.
 */
typedef struct {
  int channels;               /**< Number of channels. */
  char device[256];           /**< Target device name. */
  bool has_device;            /**< True if device is specified. */
  char node_name[256];        /**< PipeWire node name. */
  bool has_node_name;         /**< True if node_name is specified. */
  char node_description[256]; /**< PipeWire node description. */
  bool has_node_description;  /**< True if node_description is specified. */
  char node_group_name[256];  /**< PipeWire node group name. */
  bool has_node_group_name;   /**< True if node_group_name is specified. */
  char autoconnect_to[256];   /**< Node to automatically connect to. */
  bool has_autoconnect_to;    /**< True if autoconnect_to is specified. */
} pipewire_capture_config_t;

/**
 * @brief PipeWire playback configuration.
 */
typedef struct {
  int channels;               /**< Number of channels. */
  char device[256];           /**< Target device name. */
  bool has_device;            /**< True if device is specified. */
  char node_name[256];        /**< PipeWire node name. */
  bool has_node_name;         /**< True if node_name is specified. */
  char node_description[256]; /**< PipeWire node description. */
  bool has_node_description;  /**< True if node_description is specified. */
  char node_group_name[256];  /**< PipeWire node group name. */
  bool has_node_group_name;   /**< True if node_group_name is specified. */
  char autoconnect_to[256];   /**< Node to automatically connect to. */
  bool has_autoconnect_to;    /**< True if autoconnect_to is specified. */
} pipewire_playback_config_t;
#endif

#if defined(ENABLE_JACK)
/**
 * @brief JACK capture configuration.
 */
typedef struct {
  int channels;     /**< Number of channels. */
  char device[256]; /**< Client name / ports. */
} jack_capture_config_t;

/**
 * @brief JACK playback configuration.
 */
typedef struct {
  int channels;     /**< Number of channels. */
  char device[256]; /**< Client name / ports. */
} jack_playback_config_t;
#endif

/**
 * @brief Standard input capture configuration.
 */
typedef struct {
  int channels;                  /**< Number of channels. */
  binary_sample_format_t format; /**< Sample format. */
  int extra_samples;             /**< Extra samples to read. */
  bool has_extra_samples;        /**< True if extra_samples is specified. */
  int skip_bytes;                /**< Bytes to skip at start. */
  bool has_skip_bytes;           /**< True if skip_bytes is specified. */
  int read_bytes;                /**< Max bytes to read. */
  bool has_read_bytes;           /**< True if read_bytes is specified. */
} stdin_capture_config_t;

/**
 * @brief Standard output playback configuration.
 */
typedef struct {
  int channels;                  /**< Number of channels. */
  binary_sample_format_t format; /**< Sample format. */
  bool wav_header;               /**< Write WAV header. */
  bool has_wav_header;           /**< True if wav_header is specified. */
} stdout_playback_config_t;

#if defined(ENABLE_WASAPI)
/**
 * @brief WASAPI capture configuration.
 */
typedef struct {
  int channels;                  /**< Number of channels. */
  char device[256];              /**< Device name/ID. */
  bool has_device;               /**< True if device is specified. */
  wasapi_sample_format_t format; /**< Sample format. */
  bool has_format;               /**< True if format is specified. */
  bool exclusive;                /**< Use exclusive mode. */
  bool has_exclusive;            /**< True if exclusive is specified. */
  bool loopback;                 /**< Loopback capture (record output). */
  bool has_loopback;             /**< True if loopback is specified. */
  bool polling;                  /**< Use polling event mechanism. */
  bool has_polling;              /**< True if polling is specified. */
} wasapi_capture_config_t;

/**
 * @brief WASAPI playback configuration.
 */
typedef struct {
  int channels;                  /**< Number of channels. */
  char device[256];              /**< Device name/ID. */
  bool has_device;               /**< True if device is specified. */
  wasapi_sample_format_t format; /**< Sample format. */
  bool has_format;               /**< True if format is specified. */
  bool exclusive;                /**< Use exclusive mode. */
  bool has_exclusive;            /**< True if exclusive is specified. */
  bool polling;                  /**< Use polling event mechanism. */
  bool has_polling;              /**< True if polling is specified. */
} wasapi_playback_config_t;
#endif

#if defined(ENABLE_ASIO)
/**
 * @brief ASIO capture configuration.
 */
typedef struct {
  int channels;                /**< Number of channels. */
  char device[256];            /**< Device name. */
  asio_sample_format_t format; /**< Sample format. */
  bool has_format;             /**< True if format is specified. */
} asio_capture_config_t;

/**
 * @brief ASIO playback configuration.
 */
typedef struct {
  int channels;                /**< Number of channels. */
  char device[256];            /**< Device name. */
  asio_sample_format_t format; /**< Sample format. */
  bool has_format;             /**< True if format is specified. */
} asio_playback_config_t;
#endif

#if defined(ENABLE_BLUEZ)
/**
 * @brief Bluez (Bluetooth) capture configuration.
 */
typedef struct {
  char service[256];             /**< D-Bus service name. */
  bool has_service;              /**< True if service is specified. */
  char dbus_path[256];           /**< D-Bus object path. */
  bool has_dbus_path;            /**< True if dbus_path is specified. */
  binary_sample_format_t format; /**< Sample format. */
  int channels;                  /**< Number of channels. */
} bluez_capture_config_t;
#endif

/**
 * @brief WAV file capture configuration.
 */
typedef struct {
  char filename[512];     /**< Path to WAV file. */
  bool has_filename;      /**< True if filename is specified. */
  int extra_samples;      /**< Extra samples to read. */
  bool has_extra_samples; /**< True if extra_samples is specified. */
} wav_file_capture_config_t;

/**
 * @brief Raw file capture configuration.
 */
typedef struct {
  char filename[512];            /**< Path to raw file. */
  bool has_filename;             /**< True if filename is specified. */
  binary_sample_format_t format; /**< Sample format. */
  bool has_format;               /**< True if format is specified. */
  int channels;                  /**< Number of channels. */
  int skip_bytes;                /**< Bytes to skip at start. */
  bool has_skip_bytes;           /**< True if skip_bytes is specified. */
  int read_bytes;                /**< Max bytes to read. */
  bool has_read_bytes;           /**< True if read_bytes is specified. */
  int extra_samples;             /**< Extra samples to read. */
  bool has_extra_samples;        /**< True if extra_samples is specified. */
} raw_file_capture_config_t;

/**
 * @brief Raw file playback configuration.
 */
typedef struct {
  char filename[512];            /**< Path to raw file. */
  bool has_filename;             /**< True if filename is specified. */
  binary_sample_format_t format; /**< Sample format. */
  bool has_format;               /**< True if format is specified. */
  int channels;                  /**< Number of channels. */
  bool wav_header;               /**< Write WAV header. */
  bool has_wav_header;           /**< True if wav_header is specified. */
} raw_file_playback_config_t;

/**
 * @brief Signal generator capture configuration.
 */
typedef struct {
  int channels;              /**< Number of channels. */
  generator_signal_t signal; /**< Signal parameters. */
} generator_capture_config_t;

/**
 * @brief Generic capture device configuration.
 *
 * Wraps backend-specific configuration in a union.
 */
typedef struct {
  audio_backend_type_t type; /**< Audio backend type. */
  char** labels;             /**< Optional labels for channels. */
  size_t labels_count;       /**< Number of labels. */
  bool has_labels;           /**< True if labels are specified. */
  bool is_wav;               /**< True if source is a WAV file. */
  bool has_is_wav;           /**< True if is_wav is specified. */
  bool bypass_dop;           /**< True to bypass DoP decoding. */
  bool has_bypass_dop;       /**< True if bypass_dop is specified. */
  double dop_cutoff_hz;      /**< Cutoff frequency for DoP filter. */
  bool has_dop_cutoff_hz;    /**< True if dop_cutoff_hz is specified. */
  union {
#if defined(ENABLE_COREAUDIO)
    coreaudio_capture_config_t coreaudio; /**< CoreAudio config. */
#endif
#if defined(ENABLE_ALSA)
    alsa_capture_config_t alsa; /**< ALSA config. */
#endif
#if defined(ENABLE_PULSE)
    pulse_capture_config_t pulse; /**< PulseAudio config. */
#endif
#if defined(ENABLE_PIPEWIRE)
    pipewire_capture_config_t pipewire; /**< PipeWire config. */
#endif
#if defined(ENABLE_JACK)
    jack_capture_config_t jack; /**< JACK config. */
#endif
    raw_file_capture_config_t raw_file;   /**< Raw file config. */
    wav_file_capture_config_t wav_file;   /**< WAV file config. */
    stdin_capture_config_t stdin_in;      /**< STDIN config. */
    generator_capture_config_t generator; /**< Signal generator config. */
#if defined(ENABLE_WASAPI)
    wasapi_capture_config_t wasapi; /**< WASAPI config. */
#endif
#if defined(ENABLE_ASIO)
    asio_capture_config_t asio; /**< ASIO config. */
#endif
#if defined(ENABLE_BLUEZ)
    bluez_capture_config_t bluez; /**< Bluez config. */
#endif
  } cfg; /**< Backend-specific configuration union. */
} capture_device_config_t;

/**
 * @brief Generic playback device configuration.
 *
 * Wraps backend-specific configuration in a union.
 */
typedef struct {
  audio_backend_type_t type;       /**< Audio backend type. */
  char** labels;                   /**< Optional labels for channels. */
  size_t labels_count;             /**< Number of labels. */
  bool has_labels;                 /**< True if labels are specified. */
  bool is_wav;                     /**< True if destination is a WAV file. */
  bool has_is_wav;                 /**< True if is_wav is specified. */
  bool output_dop;                 /**< Enable DoP output. */
  bool has_output_dop;             /**< True if output_dop is specified. */
  sdm_filter_t dop_encoder_filter; /**< SDM filter for DoP encoding. */
  bool has_dop_encoder_filter; /**< True if dop_encoder_filter is specified. */
  union {
#if defined(ENABLE_COREAUDIO)
    coreaudio_playback_config_t coreaudio; /**< CoreAudio config. */
#endif
#if defined(ENABLE_ALSA)
    alsa_playback_config_t alsa; /**< ALSA config. */
#endif
#if defined(ENABLE_PULSE)
    pulse_playback_config_t pulse; /**< PulseAudio config. */
#endif
#if defined(ENABLE_PIPEWIRE)
    pipewire_playback_config_t pipewire; /**< PipeWire config. */
#endif
#if defined(ENABLE_JACK)
    jack_playback_config_t jack; /**< JACK config. */
#endif
    raw_file_playback_config_t raw_file; /**< Raw file config. */
    stdout_playback_config_t stdout_out; /**< STDOUT config. */
#if defined(ENABLE_WASAPI)
    wasapi_playback_config_t wasapi; /**< WASAPI config. */
#endif
#if defined(ENABLE_ASIO)
    asio_playback_config_t asio; /**< ASIO config. */
#endif
  } cfg; /**< Backend-specific configuration union. */
} playback_device_config_t;

/**
 * @brief Devices configuration (capture, playback, resampler, etc.).
 */
typedef struct {
  size_t samplerate;            /**< Playback sample rate. */
  size_t chunksize;             /**< Buffer chunk size (frames). */
  bool enable_rate_adjust;      /**< Enable automatic sample rate adjustment. */
  bool has_enable_rate_adjust;  /**< True if enable_rate_adjust is specified. */
  int target_level;             /**< Target buffer level for rate adjust. */
  bool has_target_level;        /**< True if target_level is specified. */
  double adjust_period;         /**< Rate adjustment period (seconds). */
  bool has_adjust_period;       /**< True if adjust_period is specified. */
  resampler_config_t resampler; /**< Resampler configuration. */
  bool has_resampler;           /**< True if resampler is specified. */
  capture_device_config_t capture;   /**< Capture device configuration. */
  playback_device_config_t playback; /**< Playback device configuration. */
  size_t capture_samplerate;         /**< Capture sample rate (if different from
                                        playback). */
  bool has_capture_samplerate; /**< True if capture_samplerate is specified. */
  double
      silence_threshold; /**< Silence detection threshold (dB). 0 = disabled. */
  bool has_silence_threshold; /**< True if silence_threshold is specified. */
  double silence_timeout;     /**< Silence detection timeout (seconds). 0 =
                                 disabled. */
  bool has_silence_timeout;   /**< True if silence_timeout is specified. */
  double
      volume_ramp_time; /**< Volume ramp time (milliseconds) for mute/unmute. */
  bool has_volume_ramp_time; /**< True if volume_ramp_time is specified. */
  double volume_limit;       /**< Maximum volume limit (dB). */
  bool has_volume_limit;     /**< True if volume_limit is specified. */
  int queuelimit;            /**< Queue limit for rate adjustment. */
  bool has_queuelimit;       /**< True if queuelimit is specified. */
  bool stop_on_rate_change;  /**< Stop engine if rate change is detected. */
  bool
      has_stop_on_rate_change; /**< True if stop_on_rate_change is specified. */
  double rate_measure_interval;   /**< Interval for measuring rate (seconds). */
  bool has_rate_measure_interval; /**< True if rate_measure_interval is
                                     specified. */
  bool multithreaded;             /**< Use multithreaded processing. */
  bool has_multithreaded;         /**< True if multithreaded is specified. */
  int worker_threads;             /**< Number of worker threads. */
  bool has_worker_threads;        /**< True if worker_threads is specified. */
} devices_config_t;

/**
 * @brief Initializes a capture device configuration with default values.
 * @param config Pointer to the configuration struct.
 * @param type The audio backend type.
 * @param channels Number of channels.
 */
void capture_device_config_init(capture_device_config_t* config,
                                audio_backend_type_t type, int channels);

/**
 * @brief Initializes a playback device configuration with default values.
 * @param config Pointer to the configuration struct.
 * @param type The audio backend type.
 * @param channels Number of channels.
 */
void playback_device_config_init(playback_device_config_t* config,
                                 audio_backend_type_t type, int channels);

/**
 * @brief Initializes a devices configuration.
 * @param config Pointer to the configuration struct.
 * @param samplerate Playback sample rate.
 * @param chunksize Buffer chunk size.
 * @param capture Capture device configuration.
 * @param playback Playback device configuration.
 */
void devices_config_init(devices_config_t* config, size_t samplerate,
                         size_t chunksize, capture_device_config_t capture,
                         playback_device_config_t playback);

// Accessor helper functions

/**
 * @brief Gets the number of channels from a capture device configuration.
 * @param config Pointer to the configuration.
 * @return Number of channels.
 */
int capture_device_config_get_channels(const capture_device_config_t* config);

/**
 * @brief Gets the device name from a capture device configuration.
 * @param config Pointer to the configuration.
 * @return Device name string, or NULL if not applicable/specified.
 */
const char* capture_device_config_get_device(
    const capture_device_config_t* config);

#if defined(ENABLE_COREAUDIO)
/**
 * @brief Gets CoreAudio sample format from a capture device configuration.
 * @param config Pointer to the configuration.
 * @return CoreAudio sample format.
 */
coreaudio_sample_format_t capture_device_config_get_format(
    const capture_device_config_t* config);
#endif

/**
 * @brief Gets bypass DoP setting from a capture device configuration.
 * @param config Pointer to the configuration.
 * @return True if DoP is bypassed.
 */
bool capture_device_config_get_bypass_dop(
    const capture_device_config_t* config);

/**
 * @brief Gets DoP cutoff frequency from a capture device configuration.
 * @param config Pointer to the configuration.
 * @return Cutoff frequency in Hz.
 */
double capture_device_config_get_dop_cutoff_hz(
    const capture_device_config_t* config);

/**
 * @brief Gets filename from a capture device configuration (for file backends).
 * @param config Pointer to the configuration.
 * @return Filename string, or NULL.
 */
const char* capture_device_config_get_filename(
    const capture_device_config_t* config);

/**
 * @brief Gets file format from a capture device configuration (for file
 * backends).
 * @param config Pointer to the configuration.
 * @return Binary sample format.
 */
binary_sample_format_t capture_device_config_get_file_format(
    const capture_device_config_t* config);

/**
 * @brief Gets extra samples from a capture device configuration.
 * @param config Pointer to the configuration.
 * @return Number of extra samples.
 */
int capture_device_config_get_extra_samples(
    const capture_device_config_t* config);

/**
 * @brief Gets skip bytes from a capture device configuration.
 * @param config Pointer to the configuration.
 * @return Number of bytes to skip.
 */
int capture_device_config_get_skip_bytes(const capture_device_config_t* config);

/**
 * @brief Gets read bytes from a capture device configuration.
 * @param config Pointer to the configuration.
 * @return Number of bytes to read.
 */
int capture_device_config_get_read_bytes(const capture_device_config_t* config);

/**
 * @brief Gets generator signal parameters from a capture device configuration.
 * @param config Pointer to the configuration.
 * @return Generator signal parameters.
 */
generator_signal_t capture_device_config_get_generator(
    const capture_device_config_t* config);

/**
 * @brief Gets the number of channels from a playback device configuration.
 * @param config Pointer to the configuration.
 * @return Number of channels.
 */
int playback_device_config_get_channels(const playback_device_config_t* config);

/**
 * @brief Gets the device name from a playback device configuration.
 * @param config Pointer to the configuration.
 * @return Device name string, or NULL.
 */
const char* playback_device_config_get_device(
    const playback_device_config_t* config);

#if defined(ENABLE_COREAUDIO)
/**
 * @brief Gets CoreAudio sample format from a playback device configuration.
 * @param config Pointer to the configuration.
 * @return CoreAudio sample format.
 */
coreaudio_sample_format_t playback_device_config_get_format(
    const playback_device_config_t* config);
#endif

/**
 * @brief Gets exclusive mode setting from a playback device configuration.
 * @param config Pointer to the configuration.
 * @return True if exclusive mode is enabled.
 */
bool playback_device_config_get_exclusive(
    const playback_device_config_t* config);

/**
 * @brief Gets output DoP setting from a playback device configuration.
 * @param config Pointer to the configuration.
 * @return True if DoP output is enabled.
 */
bool playback_device_config_get_output_dop(
    const playback_device_config_t* config);

/**
 * @brief Gets DoP encoder filter from a playback device configuration.
 * @param config Pointer to the configuration.
 * @return SDM filter type.
 */
sdm_filter_t playback_device_config_get_dop_encoder_filter(
    const playback_device_config_t* config);

/**
 * @brief Gets filename from a playback device configuration.
 * @param config Pointer to the configuration.
 * @return Filename string, or NULL.
 */
const char* playback_device_config_get_filename(
    const playback_device_config_t* config);

/**
 * @brief Gets file format from a playback device configuration.
 * @param config Pointer to the configuration.
 * @return Binary sample format.
 */
binary_sample_format_t playback_device_config_get_file_format(
    const playback_device_config_t* config);

/**
 * @brief Gets WAV header setting from a playback device configuration.
 * @param config Pointer to the configuration.
 * @return True if WAV header should be written.
 */
bool playback_device_config_get_wav_header(
    const playback_device_config_t* config);

/**
 * @brief Sets the number of channels in a capture device configuration.
 * @param config Pointer to the configuration.
 * @param channels Number of channels.
 */
void capture_device_config_set_channels(capture_device_config_t* config,
                                        int channels);

/**
 * @brief Sets extra samples in a capture device configuration.
 * @param config Pointer to the configuration.
 * @param extra_samples Number of extra samples.
 */
void capture_device_config_set_extra_samples(capture_device_config_t* config,
                                             int extra_samples);

#if defined(ENABLE_COREAUDIO)
/**
 * @brief Sets CoreAudio sample format in a capture device configuration.
 * @param config Pointer to the configuration.
 * @param format CoreAudio sample format.
 */
void capture_device_config_set_format(capture_device_config_t* config,
                                      coreaudio_sample_format_t format);
#endif

/**
 * @brief Sets file format in a capture device configuration.
 * @param config Pointer to the configuration.
 * @param format Binary sample format.
 */
void capture_device_config_set_file_format(capture_device_config_t* config,
                                           binary_sample_format_t format);

/**
 * @brief Recursively frees the memory allocated for an audio device descriptor.
 *
 * This function deallocates the top-level descriptor struct as well as all
 * dynamically allocated capability sets, channels, sample rates, and format
 * strings nested within it. Safe to call with NULL.
 *
 * @param desc Pointer to the descriptor to free.
 */
void free_audio_device_descriptor(audio_device_descriptor_t* desc);

#endif  // CLIB_CONFIG_ENGINE_CONFIG_TYPES_H
