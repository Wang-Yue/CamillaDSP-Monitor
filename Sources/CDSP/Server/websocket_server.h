/**
 * @file websocket_server.h
 * @brief WebSocket control server for CamillaDSP monitor.
 *
 * Provides a runtime control API compatible with the control protocol.
 */

#ifndef CLIB_SERVER_WEBSOCKET_SERVER_H
#define CLIB_SERVER_WEBSOCKET_SERVER_H

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

typedef struct dyn_string_s {
  char* data;
  size_t capacity;
  size_t length;
} dyn_string_t;

void dyn_string_init(dyn_string_t* ds, size_t initial_cap);
void dyn_string_free(dyn_string_t* ds);

/**
 * @brief Handle a control command text (either simple quoted string or JSON
 * object) and populate ds.
 *
 * @param server Pointer to the WebSocket server.
 * @param client_idx The index of the client session that sent the command.
 * @param command_text The raw command text received.
 * @param ds Dynamic string to write the response to.
 */
void websocket_server_handle_command(websocket_server_t* server, int client_idx,
                                     const char* command_text,
                                     dyn_string_t* ds);

// MARK: - Testing Helpers

bool websocket_server_get_client_vu_subscribed(const websocket_server_t* server,
                                               int client_idx);
double websocket_server_get_client_vu_max_rate(const websocket_server_t* server,
                                               int client_idx);
double websocket_server_get_client_vu_attack(const websocket_server_t* server,
                                             int client_idx);
double websocket_server_get_client_vu_release(const websocket_server_t* server,
                                              int client_idx);
void websocket_server_set_client_vu_subscribed(websocket_server_t* server,
                                               int client_idx, bool subscribed);

#endif  // CLIB_SERVER_WEBSOCKET_SERVER_H
