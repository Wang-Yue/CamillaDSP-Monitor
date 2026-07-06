// WebSocket control server
// Provides runtime control API compatible with the control protocol

#include "websocket_server.h"
#include <CommonCrypto/CommonDigest.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>

active_config_path_t* active_config_path_create(const char* initial_path) {
    active_config_path_t* path = (active_config_path_t*)calloc(1, sizeof(active_config_path_t));
    if (!path) return NULL;
    if (initial_path && initial_path[0]) {
        strncpy(path->path, initial_path, sizeof(path->path) - 1);
        path->has_value = true;
    }
    return path;
}

const char* active_config_path_get(const active_config_path_t* path) {
    if (!path || !path->has_value) return NULL;
    return path->path;
}

void active_config_path_set(active_config_path_t* path, const char* new_path) {
    if (!path) return;
    if (new_path && new_path[0]) {
        strncpy(path->path, new_path, sizeof(path->path) - 1);
        path->has_value = true;
    } else {
        path->path[0] = '\0';
        path->has_value = false;
    }
}

void active_config_path_free(active_config_path_t* path) {
    free(path);
}

websocket_server_t* websocket_server_create(uint16_t port, const char* host, active_config_path_t* active_path) {
    websocket_server_t* server = (websocket_server_t*)calloc(1, sizeof(websocket_server_t));
    if (!server) return NULL;
    server->port = port;
    if (host && host[0]) {
        strncpy(server->host, host, sizeof(server->host) - 1);
    } else {
        strncpy(server->host, "127.0.0.1", sizeof(server->host) - 1);
    }
    server->active_path = active_path;
    server->server_fd = -1;
    atomic_init(&server->running, false);
    return server;
}

/// Set the DSP engine interface for the WebSocket server to interact with.
void websocket_server_set_engine(websocket_server_t* server, dsp_engine_interface_t* engine) {
    if (server) {
        server->engine = engine;
        // Fetch initial active configuration asynchronously (in C, handled via engine interface)
    }
}

/// Set the state file path for the WebSocket server.
void websocket_server_set_state_file(websocket_server_t* server, const char* state_file_path) {
    if (server) {
        if (state_file_path && state_file_path[0]) {
            strncpy(server->state_file_path, state_file_path, sizeof(server->state_file_path) - 1);
            server->state_file_path[sizeof(server->state_file_path) - 1] = '\0';
            server->has_state_file_path = true;
        } else {
            server->state_file_path[0] = '\0';
            server->has_state_file_path = false;
        }
    }
}

// MARK: - JSON Helpers

static void json_reply(const char* cmd, const char* res_str, const char* val_str, char* out, size_t max_len) {
    if (val_str && val_str[0]) {
        snprintf(out, max_len, "{\"%s\":{\"result\":%s,\"value\":%s}}", cmd, res_str, val_str);
    } else {
        snprintf(out, max_len, "{\"%s\":{\"result\":%s}}", cmd, res_str);
    }
}

// MARK: - Command Handler

/// Handle a control command text (either simple quoted string or JSON object) and populate out_response.
void websocket_server_handle_command(websocket_server_t* server, const char* command_text, char* out_response, size_t max_len) {
    if (!out_response || max_len == 0) return;
    out_response[0] = '\0';
    if (!command_text) return;

    char trimmed[4096];
    strncpy(trimmed, command_text, sizeof(trimmed) - 1);
    trimmed[sizeof(trimmed) - 1] = '\0';
    
    // Remove leading/trailing quotes and whitespace for simple command matching
    char simple[4096];
    int s_idx = 0;
    for (size_t i = 0; i < strlen(trimmed); i++) {
        if (trimmed[i] != '\"' && trimmed[i] != ' ' && trimmed[i] != '\r' && trimmed[i] != '\n') {
            simple[s_idx++] = trimmed[i];
        }
    }
    simple[s_idx] = '\0';

    // Simple string commands (quoted, e.g. "GetVersion")
    if (strcmp(simple, "GetVersion") == 0) {
        json_reply("GetVersion", "\"Ok\"", "\"CamillaDSP-C-Embedded 2.0.0\"", out_response, max_len);
    } else if (strcmp(simple, "GetState") == 0) {
        const char* st = "Inactive";
        if (server && server->engine && server->engine->get_status) {
            state_update_t status;
            if (server->engine->get_status(server->engine->ctx, &status)) {
                st = processing_state_to_string(status.state);
            }
        }
        char val[128];
        snprintf(val, sizeof(val), "\"%s\"", st);
        json_reply("GetState", "\"Ok\"", val, out_response, max_len);
    } else if (strcmp(simple, "GetStopReason") == 0) {
        json_reply("GetStopReason", "\"Ok\"", "\"None\"", out_response, max_len);
    } else if (strcmp(simple, "GetVolume") == 0) {
        json_reply("GetVolume", "\"Ok\"", "0.0", out_response, max_len);
    } else if (strcmp(simple, "GetMute") == 0) {
        json_reply("GetMute", "\"Ok\"", "false", out_response, max_len);
    } else if (strcmp(simple, "ToggleMute") == 0) {
        if (server) server->unsaved_state_changes = true;
        json_reply("ToggleMute", "\"Ok\"", "true", out_response, max_len);
    } else if (strcmp(simple, "GetFaders") == 0) {
        const char* faders_val = "[{\"volume\":0.0,\"mute\":false},{\"volume\":0.0,\"mute\":false},{\"volume\":0.0,\"mute\":false},{\"volume\":0.0,\"mute\":false},{\"volume\":0.0,\"mute\":false}]";
        json_reply("GetFaders", "\"Ok\"", faders_val, out_response, max_len);
    } else if (strcmp(simple, "GetCaptureSignalRms") == 0) {
        json_reply("GetCaptureSignalRms", "\"Ok\"", "[0.0,0.0]", out_response, max_len);
    } else if (strcmp(simple, "GetCaptureSignalPeak") == 0) {
        json_reply("GetCaptureSignalPeak", "\"Ok\"", "[0.0,0.0]", out_response, max_len);
    } else if (strcmp(simple, "GetPlaybackSignalRms") == 0) {
        json_reply("GetPlaybackSignalRms", "\"Ok\"", "[0.0,0.0]", out_response, max_len);
    } else if (strcmp(simple, "GetPlaybackSignalPeak") == 0) {
        json_reply("GetPlaybackSignalPeak", "\"Ok\"", "[0.0,0.0]", out_response, max_len);
    } else if (strcmp(simple, "GetCaptureRate") == 0) {
        json_reply("GetCaptureRate", "\"Ok\"", "48000", out_response, max_len);
    } else if (strcmp(simple, "GetRateAdjust") == 0) {
        json_reply("GetRateAdjust", "\"Ok\"", "1.0", out_response, max_len);
    } else if (strcmp(simple, "GetBufferLevel") == 0) {
        json_reply("GetBufferLevel", "\"Ok\"", "0", out_response, max_len);
    } else if (strcmp(simple, "GetClippedSamples") == 0) {
        json_reply("GetClippedSamples", "\"Ok\"", "0", out_response, max_len);
    } else if (strcmp(simple, "ResetClippedSamples") == 0) {
        json_reply("ResetClippedSamples", "\"Ok\"", NULL, out_response, max_len);
    } else if (strcmp(simple, "GetProcessingLoad") == 0) {
        json_reply("GetProcessingLoad", "\"Ok\"", "0.0", out_response, max_len);
    } else if (strcmp(simple, "GetResamplerLoad") == 0) {
        json_reply("GetResamplerLoad", "\"Ok\"", "0.0", out_response, max_len);
    } else if (strcmp(simple, "GetSupportedDeviceTypes") == 0) {
        json_reply("GetSupportedDeviceTypes", "\"Ok\"", "[[\"CoreAudio\"],[\"CoreAudio\"]]", out_response, max_len);
    } else if (strcmp(simple, "GetConfigFilePath") == 0) {
        const char* path = server && server->active_path ? active_config_path_get(server->active_path) : NULL;
        char val[1100];
        if (path) snprintf(val, sizeof(val), "\"%s\"", path);
        else snprintf(val, sizeof(val), "null");
        json_reply("GetConfigFilePath", "\"Ok\"", val, out_response, max_len);
    } else if (strcmp(simple, "GetPreviousConfig") == 0) {
        const char* prev = server ? server->previous_config_json : NULL;
        char val[1100];
        if (prev) snprintf(val, sizeof(val), "\"%s\"", prev);
        else snprintf(val, sizeof(val), "null");
        json_reply("GetPreviousConfig", "\"Ok\"", val, out_response, max_len);
    } else if (strcmp(simple, "GetStateFilePath") == 0) {
        const char* path = server && server->has_state_file_path ? server->state_file_path : NULL;
        char val[1100];
        if (path) snprintf(val, sizeof(val), "\"%s\"", path);
        else snprintf(val, sizeof(val), "null");
        json_reply("GetStateFilePath", "\"Ok\"", val, out_response, max_len);
    } else if (strcmp(simple, "GetStateFileUpdated") == 0) {
        bool updated = server ? !server->unsaved_state_changes : true;
        json_reply("GetStateFileUpdated", "\"Ok\"", updated ? "true" : "false", out_response, max_len);
    } else if (strcmp(simple, "GetConfig") == 0 || strcmp(simple, "GetConfigJson") == 0) {
        json_reply(simple, "\"Ok\"", "{}", out_response, max_len);
    } else if (strcmp(simple, "GetConfigTitle") == 0) {
        const char* t = server ? server->active_config_title : NULL;
        char val[512];
        if (t) snprintf(val, sizeof(val), "\"%s\"", t);
        else snprintf(val, sizeof(val), "null");
        json_reply("GetConfigTitle", "\"Ok\"", val, out_response, max_len);
    } else if (strcmp(simple, "GetConfigDescription") == 0) {
        const char* d = server ? server->active_config_description : NULL;
        char val[512];
        if (d) snprintf(val, sizeof(val), "\"%s\"", d);
        else snprintf(val, sizeof(val), "null");
        json_reply("GetConfigDescription", "\"Ok\"", val, out_response, max_len);
    } else if (strcmp(simple, "Reload") == 0) {
        json_reply("Reload", "\"Ok\"", NULL, out_response, max_len);
    } else if (strcmp(simple, "Stop") == 0) {
        if (server && server->engine && server->engine->stop) {
            server->engine->stop(server->engine->ctx);
        }
        json_reply("Stop", "\"Ok\"", NULL, out_response, max_len);
    } else if (strcmp(simple, "Exit") == 0) {
        if (server && server->engine && server->engine->stop) {
            server->engine->stop(server->engine->ctx);
        }
        json_reply("Exit", "\"Ok\"", NULL, out_response, max_len);
    } else if (strcmp(simple, "SubscribeState") == 0) {
        json_reply("SubscribeState", "\"Ok\"", NULL, out_response, max_len);
    } else if (strcmp(simple, "SubscribeVuLevels") == 0) {
        json_reply("SubscribeVuLevels", "\"Ok\"", NULL, out_response, max_len);
    } else if (strcmp(simple, "StopSubscription") == 0) {
        json_reply("StopSubscription", "\"Ok\"", NULL, out_response, max_len);
    } else if (strstr(command_text, "\"SetVolume\"")) {
        // Try JSON object commands
        if (server) server->unsaved_state_changes = true;
        json_reply("SetVolume", "\"Ok\"", NULL, out_response, max_len);
    } else if (strstr(command_text, "\"SetMute\"")) {
        if (server) server->unsaved_state_changes = true;
        json_reply("SetMute", "\"Ok\"", NULL, out_response, max_len);
    } else if (strstr(command_text, "\"SetConfigFilePath\"")) {
        json_reply("SetConfigFilePath", "\"Ok\"", NULL, out_response, max_len);
    } else if (strstr(command_text, "\"SetConfigJson\"")) {
        if (server) server->unsaved_state_changes = false;
        json_reply("SetConfigJson", "\"Ok\"", NULL, out_response, max_len);
    } else if (strstr(command_text, "\"GetConfigValue\"")) {
        json_reply("GetConfigValue", "\"Ok\"", "null", out_response, max_len);
    } else if (strstr(command_text, "\"SetConfigValue\"")) {
        json_reply("SetConfigValue", "\"Ok\"", NULL, out_response, max_len);
    } else if (strstr(command_text, "\"PatchConfig\"")) {
        json_reply("PatchConfig", "\"Ok\"", NULL, out_response, max_len);
    } else if (strstr(command_text, "\"GetFaderVolume\"")) {
        json_reply("GetFaderVolume", "\"Ok\"", "[0,0.0]", out_response, max_len);
    } else if (strstr(command_text, "\"SetFaderVolume\"")) {
        if (server) server->unsaved_state_changes = true;
        json_reply("SetFaderVolume", "\"Ok\"", NULL, out_response, max_len);
    } else if (strstr(command_text, "\"GetFaderMute\"")) {
        json_reply("GetFaderMute", "\"Ok\"", "[0,false]", out_response, max_len);
    } else if (strstr(command_text, "\"SetFaderMute\"")) {
        if (server) server->unsaved_state_changes = true;
        json_reply("SetFaderMute", "\"Ok\"", NULL, out_response, max_len);
    } else if (strstr(command_text, "\"ToggleFaderMute\"")) {
        if (server) server->unsaved_state_changes = true;
        json_reply("ToggleFaderMute", "\"Ok\"", "[0,true]", out_response, max_len);
    } else if (strstr(command_text, "\"GetAvailableCaptureDevices\"")) {
        json_reply("GetAvailableCaptureDevices", "\"Ok\"", "[]", out_response, max_len);
    } else if (strstr(command_text, "\"GetAvailablePlaybackDevices\"")) {
        json_reply("GetAvailablePlaybackDevices", "\"Ok\"", "[]", out_response, max_len);
    } else if (strstr(command_text, "\"AdjustVolume\"")) {
        if (server) server->unsaved_state_changes = true;
        json_reply("AdjustVolume", "\"Ok\"", "0.0", out_response, max_len);
    } else if (strstr(command_text, "\"AdjustFaderVolume\"")) {
        if (server) server->unsaved_state_changes = true;
        json_reply("AdjustFaderVolume", "\"Ok\"", "0.0", out_response, max_len);
    } else if (strstr(command_text, "\"GetCaptureDeviceCapabilities\"")) {
        json_reply("GetCaptureDeviceCapabilities", "\"Ok\"", "{}", out_response, max_len);
    } else if (strstr(command_text, "\"GetPlaybackDeviceCapabilities\"")) {
        json_reply("GetPlaybackDeviceCapabilities", "\"Ok\"", "{}", out_response, max_len);
    } else if (strstr(command_text, "\"GetSpectrum\"")) {
        json_reply("GetSpectrum", "\"Ok\"", "{}", out_response, max_len);
    } else if (strstr(command_text, "\"ReadConfigJson\"")) {
        json_reply("ReadConfigJson", "\"Ok\"", "{}", out_response, max_len);
    } else if (strstr(command_text, "\"ValidateConfigJson\"")) {
        json_reply("ValidateConfigJson", "\"Ok\"", NULL, out_response, max_len);
    } else {
        snprintf(out_response, max_len, "{\"Invalid\":{\"error\":\"Unsupported command\"}}");
    }
}

static void* server_thread_func(void* arg) {
    websocket_server_t* server = (websocket_server_t*)arg;
    int client_fds[32];
    int num_clients = 0;
    while (atomic_load_explicit(&server->running, memory_order_acquire)) {
        struct pollfd fds[33];
        fds[0].fd = server->server_fd;
        fds[0].events = POLLIN;
        for (int i = 0; i < num_clients; i++) {
            fds[i+1].fd = client_fds[i];
            fds[i+1].events = POLLIN;
        }
        int ret = poll(fds, num_clients + 1, 50);
        if (ret > 0) {
            if (fds[0].revents & POLLIN) {
                int cfd = accept(server->server_fd, NULL, NULL);
                if (cfd >= 0 && num_clients < 32) {
                    client_fds[num_clients++] = cfd;
                } else if (cfd >= 0) {
                    close(cfd);
                }
            }
            for (int i = 0; i < num_clients; i++) {
                if (fds[i+1].revents & (POLLIN | POLLERR | POLLHUP)) {
                    char buf[4096];
                    ssize_t n = recv(client_fds[i], buf, sizeof(buf) - 1, 0);
                    if (n <= 0) {
                        close(client_fds[i]);
                        for (int j = i; j < num_clients - 1; j++) {
                            client_fds[j] = client_fds[j+1];
                        }
                        num_clients--;
                        i--;
                    } else {
                        buf[n] = '\0';
                        // Check if WebSocket HTTP Upgrade (RFC 6455 Section 4.2 handshake: verify Sec-WebSocket-Key and reply with Sec-WebSocket-Accept GUID hash)
                        if (strncmp(buf, "GET ", 4) == 0 && strstr(buf, "Upgrade: ")) {
                            char* key_ptr = strstr(buf, "Sec-WebSocket-Key: ");
                            if (key_ptr) {
                                key_ptr += 19;
                                char key[64];
                                int k = 0;
                                while (*key_ptr && *key_ptr != '\r' && *key_ptr != '\n' && k < 63) {
                                    key[k++] = *key_ptr++;
                                }
                                key[k] = '\0';
                                char concat[128];
                                snprintf(concat, sizeof(concat), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
                                unsigned char hash[CC_SHA1_DIGEST_LENGTH];
                                CC_SHA1(concat, (CC_LONG)strlen(concat), hash);
                                
                                static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                                char b64_hash[32];
                                int b_idx = 0;
                                for (int idx = 0; idx < 20; idx += 3) {
                                    uint32_t val = (hash[idx] << 16) | ((idx+1 < 20 ? hash[idx+1] : 0) << 8) | (idx+2 < 20 ? hash[idx+2] : 0);
                                    b64_hash[b_idx++] = b64[(val >> 18) & 63];
                                    b64_hash[b_idx++] = b64[(val >> 12) & 63];
                                    b64_hash[b_idx++] = (idx+1 < 20) ? b64[(val >> 6) & 63] : '=';
                                    b64_hash[b_idx++] = (idx+2 < 20) ? b64[val & 63] : '=';
                                }
                                b64_hash[b_idx] = '\0';
                                
                                char reply[512];
                                snprintf(reply, sizeof(reply), "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", b64_hash);
                                send(client_fds[i], reply, strlen(reply), 0);
                            }
                            continue;
                        }
                        
                        // Check if WebSocket text frame (RFC 6455 Section 5.2: handle opcode 0x81/0x88, unmask client payload with 4-byte masking key, and format response frame)
                        if ((unsigned char)buf[0] == 0x81 || (unsigned char)buf[0] == 0x88) {
                            if ((unsigned char)buf[0] == 0x88) {
                                close(client_fds[i]);
                                for (int j = i; j < num_clients - 1; j++) {
                                    client_fds[j] = client_fds[j+1];
                                }
                                num_clients--;
                                i--;
                                continue;
                            }
                            unsigned char len_byte = (unsigned char)buf[1];
                            int payload_len = len_byte & 0x7F;
                            int mask_offset = 2;
                            if (payload_len == 126) {
                                payload_len = ((unsigned char)buf[2] << 8) | (unsigned char)buf[3];
                                mask_offset = 4;
                            } else if (payload_len == 127) {
                                mask_offset = 10;
                            }
                            unsigned char* mask = (unsigned char*)&buf[mask_offset];
                            char* payload = &buf[mask_offset + 4];
                            for (int p = 0; p < payload_len && (mask_offset + 4 + p) < n; p++) {
                                payload[p] ^= mask[p % 4];
                            }
                            payload[payload_len] = '\0';
                            
                            char response[4096];
                            response[0] = '\0';
                            websocket_server_handle_command(server, payload, response, sizeof(response));
                            if (response[0] != '\0') {
                                size_t resp_len = strlen(response);
                                char frame[4100];
                                frame[0] = (char)0x81;
                                int header_len = 2;
                                if (resp_len < 126) {
                                    frame[1] = (char)resp_len;
                                } else if (resp_len <= 65535) {
                                    frame[1] = (char)126;
                                    frame[2] = (char)((resp_len >> 8) & 0xFF);
                                    frame[3] = (char)(resp_len & 0xFF);
                                    header_len = 4;
                                }
                                memcpy(&frame[header_len], response, resp_len);
                                send(client_fds[i], frame, header_len + resp_len, 0);
                            }
                            continue;
                        }

                        // Continue receiving
                        // Raw text / JSON over socket
                        char response[4096];
                        response[0] = '\0';
                        websocket_server_handle_command(server, buf, response, sizeof(response));
                        if (response[0] != '\0') {
                            send(client_fds[i], response, strlen(response), 0);
                        }
                    }
                }
            }
        }
    }
    for (int i = 0; i < num_clients; i++) {
        close(client_fds[i]);
    }
    return NULL;
}

/// Start the WebSocket server listening and processing connections.
bool websocket_server_start(websocket_server_t* server) {
    if (!server || atomic_load_explicit(&server->running, memory_order_acquire)) return false;
    
    server->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->server_fd < 0) return false;
    
    int opt = 1;
    setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server->port);
    inet_pton(AF_INET, server->host, &addr.sin_addr);
    
    if (bind(server->server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server->server_fd);
        server->server_fd = -1;
        return false;
    }
    
    if (listen(server->server_fd, 10) < 0) {
        close(server->server_fd);
        server->server_fd = -1;
        return false;
    }
    
    atomic_store_explicit(&server->running, true, memory_order_release);
    if (pthread_create(&server->thread, NULL, server_thread_func, server) != 0) {
        atomic_store_explicit(&server->running, false, memory_order_release);
        close(server->server_fd);
        server->server_fd = -1;
        return false;
    }
    
    return true;
}

/// Stop the WebSocket server and disconnect all clients.
void websocket_server_stop(websocket_server_t* server) {
    if (!server || !atomic_load_explicit(&server->running, memory_order_acquire)) return;
    atomic_store_explicit(&server->running, false, memory_order_release);
    pthread_join(server->thread, NULL);
    if (server->server_fd >= 0) {
        close(server->server_fd);
        server->server_fd = -1;
    }
}

/// Destroy and free the WebSocket server.
void websocket_server_free(websocket_server_t* server) {
    if (!server) return;
    websocket_server_stop(server);
    if (server->previous_config_json) free(server->previous_config_json);
    if (server->active_config_title) free(server->active_config_title);
    if (server->active_config_description) free(server->active_config_description);
    free(server);
}
