/**
 * @file websocket_server.h
 * @brief WebSocket control server for CamillaDSP monitor.
 *
 * Provides a runtime control API compatible with the control protocol.
 */

#ifndef CLIB_SERVER_WEBSOCKET_SERVER_H
#define CLIB_SERVER_WEBSOCKET_SERVER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Backend/backend_error.h"
#include "Config/configuration.h"
#include "Config/engine_config_types.h"

/**
 * @brief Opaque structure representing a WebSocket server.
 */
typedef struct websocket_server websocket_server_t;

/**
 * @brief Interface for DSPEngine that WebSocketServer interacts with.
 *
 * This structure contains function pointers that allow the WebSocket server to
 * query status, get configurations, retrieve levels, and control the DSP
 * engine.
 */
typedef struct {
  /** Context pointer passed to all callback functions. */
  void* ctx;
  /** Gets the current state update/status. */
  bool (*get_status)(void* ctx, state_update_t* out_status);
  /** Gets current processing parameters. */
  bool (*get_processing_parameters)(void* ctx, void** out_params);
  /** Gets the active configuration as a JSON string. */
  bool (*get_active_config_json)(void* ctx, char** out_json);
  /** Gets the previous configuration as a JSON string. */
  bool (*get_previous_config_json)(void* ctx, char** out_json);
  /** Gets the active DSP configuration struct. */
  const dsp_config_t* (*get_active_config)(void* ctx);
  /** Gets the current VU levels. */
  bool (*get_vu_levels)(void* ctx, vu_levels_t* out_vu);
  /** Gets available audio devices. */
  bool (*get_available_devices)(void* ctx, const char* backend, bool is_input,
                                audio_device_t** out_devices,
                                size_t* out_count);
  /** Gets capabilities for a specific device. */
  bool (*get_device_capabilities)(void* ctx, const char* backend,
                                  const char* device, bool is_capture,
                                  audio_device_descriptor_t** out_desc,
                                  device_error_t* out_err);
  /** Gets spectrum data. */
  bool (*get_spectrum)(void* ctx, bool is_capture, uint32_t channel,
                       double min_freq, double max_freq, uint32_t n_bins,
                       spectrum_t* out_spec);
  /** Sets the active configuration using a JSON string. */
  bool (*set_config_json)(void* ctx, const char* json_str,
                          audio_backend_error_t* out_err);
  /** Stops the DSP engine. */
  void (*stop)(void* ctx);
  /** Sets volume of a fader. */
  void (*set_fader_volume)(void* ctx, fader_t fader, float db, bool instant);
  /** Mutes or unmutes a fader. */
  void (*set_fader_mute)(void* ctx, fader_t fader, bool mute);

  // Path & persistence callbacks
  /** Gets the path to the state file. */
  const char* (*get_state_file)(void* ctx);
  /** Checks if the current state is dirty and needs saving. */
  bool (*is_state_dirty)(void* ctx);
  /** Gets the path to the configuration file. */
  char* (*get_config_path)(void* ctx);
  /** Sets the path to the configuration file. */
  void (*set_config_path)(void* ctx, const char* path);
} dsp_engine_interface_t;

/**
 * @brief Represents a single point in level history.
 */
typedef struct {
  /** Timestamp in milliseconds when the level was sampled. */
  uint64_t timestamp_ms;
  /** Array of levels (one per channel). */
  double* levels;
} level_sample_t;

/**
 * @brief Stores historical level samples.
 */
typedef struct {
  /** Array of historical level samples. */
  level_sample_t samples[300];
  /** Index of the head of the circular buffer. */
  size_t head;
  /** Number of elements in the buffer. */
  size_t size;
  /** Number of channels per sample. */
  size_t channels;
} level_history_t;

/**
 * @brief Represents a single client's WebSocket session state.
 */
typedef struct {
  /** Last time capture peak levels were pushed. */
  uint64_t last_cap_peak_time;
  /** Last time capture RMS levels were pushed. */
  uint64_t last_cap_rms_time;
  /** Last time playback peak levels were pushed. */
  uint64_t last_pb_peak_time;
  /** Last time playback RMS levels were pushed. */
  uint64_t last_pb_rms_time;

  /** True if client is subscribed to state updates. */
  bool state_subscribed;
  /** True if client is subscribed to VU level updates. */
  bool vu_subscribed;
  /** True if client is subscribed to signal level updates. */
  bool signal_levels_subscribed;
  /** Side to subscribe to for signal levels ("capture" or "playback"). */
  char signal_levels_side[16];
  /** True if client is subscribed to spectrum updates. */
  bool spectrum_subscribed;
  /** True if spectrum subscription is for capture channels. */
  bool spectrum_is_capture;
  /** Channel index for spectrum updates. */
  uint32_t spectrum_channel;
  /** Minimum frequency for spectrum updates. */
  double spectrum_min_freq;
  /** Maximum frequency for spectrum updates. */
  double spectrum_max_freq;
  /** Number of bins for spectrum updates. */
  uint32_t spectrum_n_bins;
  /** Maximum rate of spectrum updates (per second). */
  double spectrum_max_rate;
  /** Last time spectrum data was pushed to this client. */
  uint64_t last_spectrum_push_time;

  /** Maximum rate of VU updates (per second). */
  double vu_max_rate;
  /** Attack time for VU meters. */
  double vu_attack;
  /** Release time for VU meters. */
  double vu_release;

  /** Last time VU levels were pushed to this client. */
  uint64_t last_vu_push_time;

  /** Current playback RMS levels. */
  double* vu_pb_rms;
  /** Current playback peak levels. */
  double* vu_pb_peak;
  /** Current capture RMS levels. */
  double* vu_cap_rms;
  /** Current capture peak levels. */
  double* vu_cap_peak;
  /** Number of playback channels in the VU. */
  size_t vu_pb_channels;
  /** Number of capture channels in the VU. */
  size_t vu_cap_channels;
} client_session_t;

/**
 * @brief Structure containing the internal state of the WebSocket server.
 */
struct websocket_server {
  /** The port the server listens on. */
  uint16_t port;
  /** The host interface the server binds to. */
  char host[128];
  /** The interface to the DSP engine. */
  dsp_engine_interface_t* engine;

  /** Server socket file descriptor. */
  int server_fd;
  /** Atomic flag indicating if the server is running. */
  _Atomic bool running;
  /** Thread handle for the server runloop. */
  pthread_t thread;

  /** Server update/tick interval in microseconds. */
  uint32_t update_interval;

  /** History of capture peak levels. */
  level_history_t capture_peak_history;
  /** History of capture RMS levels. */
  level_history_t capture_rms_history;
  /** History of playback peak levels. */
  level_history_t playback_peak_history;
  /** History of playback RMS levels. */
  level_history_t playback_rms_history;

  /** Array storing global peak capture levels per channel. */
  double* capture_global_peaks;
  /** Array storing global peak playback levels per channel. */
  double* playback_global_peaks;
  /** Number of capture channels for global peaks. */
  size_t capture_global_peaks_count;
  /** Number of playback channels for global peaks. */
  size_t playback_global_peaks_count;

  /** Array of active client sessions. */
  client_session_t client_sessions[32];
};

/**
 * @brief Create a new WebSocket control server on the specified port and host.
 *
 * @param port Port number to listen on.
 * @param host Hostname or IP address to bind to.
 * @return A pointer to the created websocket_server_t, or NULL on failure.
 */
websocket_server_t* websocket_server_create(uint16_t port, const char* host);

/**
 * @brief Set the DSP engine interface for the WebSocket server to interact
 * with.
 *
 * @param server Pointer to the WebSocket server.
 * @param engine Pointer to the DSP engine interface.
 */
void websocket_server_set_engine(websocket_server_t* server,
                                 dsp_engine_interface_t* engine);

/**
 * @brief Start the WebSocket server listening and processing connections in a
 * background thread.
 *
 * @param server Pointer to the WebSocket server.
 * @return true if the server started successfully, false otherwise.
 */
bool websocket_server_start(websocket_server_t* server);

/**
 * @brief Stop the WebSocket server, disconnect all clients, and join the server
 * thread.
 *
 * @param server Pointer to the WebSocket server.
 */
void websocket_server_stop(websocket_server_t* server);

/**
 * @brief Destroy and free the WebSocket server.
 *
 * @param server Pointer to the WebSocket server to free.
 */
void websocket_server_free(websocket_server_t* server);

// MARK: - Command Handler

/**
 * @brief Handle a control command text (either simple quoted string or JSON
 * object) and populate out_response.
 *
 * @param server Pointer to the WebSocket server.
 * @param client_idx The index of the client session that sent the command.
 * @param command_text The raw command text received.
 * @param out_response Buffer to write the response to.
 * @param max_len Maximum length of the response buffer.
 */
void websocket_server_handle_command(websocket_server_t* server, int client_idx,
                                     const char* command_text,
                                     char* out_response, size_t max_len);

#endif  // CLIB_SERVER_WEBSOCKET_SERVER_H
