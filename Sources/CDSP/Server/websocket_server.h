// WebSocket control server
// Provides runtime control API compatible with the control protocol

#ifndef CLIB_SERVER_WEBSOCKET_SERVER_H
#define CLIB_SERVER_WEBSOCKET_SERVER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Config/configuration.h"
#include "Config/engine_config_types.h"

typedef struct active_config_path active_config_path_t;
typedef struct websocket_server websocket_server_t;

/// Create an active configuration path wrapper.
active_config_path_t* active_config_path_create(const char* initial_path);
/// Get the current path string from the active configuration path wrapper.
const char* active_config_path_get(const active_config_path_t* path);
/// Set a new path string on the active configuration path wrapper.
void active_config_path_set(active_config_path_t* path, const char* new_path);
/// Free the active configuration path wrapper.
void active_config_path_free(active_config_path_t* path);

// Interface for DSPEngine that WebSocketServer interacts with
typedef struct {
  void* ctx;
  bool (*get_status)(void* ctx, state_update_t* out_status);
  bool (*get_processing_parameters)(void* ctx, void** out_params);
  bool (*get_active_config_json)(void* ctx, char** out_json);
  const dsp_config_t* (*get_active_config)(void* ctx);
  bool (*get_vu_levels)(void* ctx, vu_levels_t* out_vu);
  bool (*get_available_devices)(void* ctx, const char* backend, bool is_input,
                                audio_device_t** out_devices,
                                size_t* out_count);
  bool (*get_device_capabilities)(void* ctx, const char* backend,
                                  const char* device, bool is_capture,
                                  audio_device_descriptor_t** out_desc);
  bool (*get_spectrum)(void* ctx, bool is_capture, uint32_t channel,
                       double min_freq, double max_freq, uint32_t n_bins,
                       spectrum_t* out_spec);
  bool (*set_config_json)(void* ctx, const char* json_str,
                          audio_backend_error_t* out_err);
  void (*stop)(void* ctx);
} dsp_engine_interface_t;

struct active_config_path {
  char path[1024];
  bool has_value;
};

typedef struct {
  uint64_t timestamp_ms;
  double* levels;
} level_sample_t;

typedef struct {
  level_sample_t samples[300];
  size_t head;
  size_t size;
  size_t channels;
} level_history_t;

typedef struct {
  uint64_t last_cap_peak_time;
  uint64_t last_cap_rms_time;
  uint64_t last_pb_peak_time;
  uint64_t last_pb_rms_time;

  bool state_subscribed;
  bool vu_subscribed;
  bool signal_levels_subscribed;
  char signal_levels_side[16];
  bool spectrum_subscribed;
  bool spectrum_is_capture;
  uint32_t spectrum_channel;
  double spectrum_min_freq;
  double spectrum_max_freq;
  uint32_t spectrum_n_bins;
  double spectrum_max_rate;
  uint64_t last_spectrum_push_time;

  double vu_max_rate;
  double vu_attack;
  double vu_release;

  uint64_t last_vu_push_time;

  double* vu_pb_rms;
  double* vu_pb_peak;
  double* vu_cap_rms;
  double* vu_cap_peak;
  size_t vu_pb_channels;
  size_t vu_cap_channels;
} client_session_t;

struct websocket_server {
  uint16_t port;
  char host[128];
  active_config_path_t* active_path;
  dsp_engine_interface_t* engine;

  int server_fd;
  _Atomic bool running;
  pthread_t thread;

  char* active_config_json;
  char* previous_config_json;
  char state_file_path[1024];
  bool has_state_file_path;
  bool unsaved_state_changes;

  char* active_config_title;
  char* active_config_description;

  uint32_t update_interval;

  level_history_t capture_peak_history;
  level_history_t capture_rms_history;
  level_history_t playback_peak_history;
  level_history_t playback_rms_history;

  double* capture_global_peaks;
  double* playback_global_peaks;
  size_t capture_global_peaks_count;
  size_t playback_global_peaks_count;

  client_session_t client_sessions[32];
};

/// Create a new WebSocket control server on the specified port and host.
websocket_server_t* websocket_server_create(uint16_t port, const char* host,
                                            active_config_path_t* active_path);
/// Set the DSP engine interface for the WebSocket server to interact with.
void websocket_server_set_engine(websocket_server_t* server,
                                 dsp_engine_interface_t* engine);
/// Set the state file path for the WebSocket server.
void websocket_server_set_state_file(websocket_server_t* server,
                                     const char* state_file_path);
/// Start the WebSocket server listening and processing connections.
bool websocket_server_start(websocket_server_t* server);
/// Stop the WebSocket server and disconnect all clients.
void websocket_server_stop(websocket_server_t* server);
/// Destroy and free the WebSocket server.
void websocket_server_free(websocket_server_t* server);

// MARK: - Command Handler

/// Handle a control command text (either simple quoted string or JSON object)
/// and populate out_response.
void websocket_server_handle_command(websocket_server_t* server, int client_idx,
                                     const char* command_text,
                                     char* out_response, size_t max_len);

#endif  // CLIB_SERVER_WEBSOCKET_SERVER_H
