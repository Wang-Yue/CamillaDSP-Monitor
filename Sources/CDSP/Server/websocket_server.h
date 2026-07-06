// WebSocket control server
// Provides runtime control API compatible with the control protocol

#ifndef CLIB_SERVER_WEBSOCKET_SERVER_H
#define CLIB_SERVER_WEBSOCKET_SERVER_H

#include "Config/engine_config_types.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    bool (*get_vu_levels)(void* ctx, vu_levels_t* out_vu);
    bool (*get_available_devices)(void* ctx, const char* backend, bool is_input, audio_device_t** out_devices, size_t* out_count);
    bool (*get_device_capabilities)(void* ctx, const char* backend, const char* device, bool is_capture, audio_device_descriptor_t** out_desc);
    bool (*get_spectrum)(void* ctx, bool is_capture, uint32_t channel, double min_freq, double max_freq, uint32_t n_bins, spectrum_t* out_spec);
    bool (*set_config_json)(void* ctx, const char* json_str, char* out_err_msg, size_t err_len);
    void (*stop)(void* ctx);
} dsp_engine_interface_t;

struct active_config_path {
    char path[1024];
    bool has_value;
};

struct websocket_server {
    uint16_t port;
    char host[128];
    active_config_path_t* active_path;
    dsp_engine_interface_t* engine;
    
    int server_fd;
    _Atomic bool running;
    pthread_t thread;
    
    char* previous_config_json;
    char state_file_path[1024];
    bool has_state_file_path;
    bool unsaved_state_changes;
    
    char* active_config_title;
    char* active_config_description;
};

/// Create a new WebSocket control server on the specified port and host.
websocket_server_t* websocket_server_create(uint16_t port, const char* host, active_config_path_t* active_path);
/// Set the DSP engine interface for the WebSocket server to interact with.
void websocket_server_set_engine(websocket_server_t* server, dsp_engine_interface_t* engine);
/// Set the state file path for the WebSocket server.
void websocket_server_set_state_file(websocket_server_t* server, const char* state_file_path);
/// Start the WebSocket server listening and processing connections.
bool websocket_server_start(websocket_server_t* server);
/// Stop the WebSocket server and disconnect all clients.
void websocket_server_stop(websocket_server_t* server);
/// Destroy and free the WebSocket server.
void websocket_server_free(websocket_server_t* server);

// MARK: - Command Handler

/// Handle a control command text (either simple quoted string or JSON object) and populate out_response.
void websocket_server_handle_command(websocket_server_t* server, const char* command_text, char* out_response, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif // CLIB_SERVER_WEBSOCKET_SERVER_H
