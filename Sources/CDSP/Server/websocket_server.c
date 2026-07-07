// WebSocket control server
// Provides runtime control API compatible with the control protocol

#define JSMN_STATIC
#include "websocket_server.h"

#ifndef _WIN32
#include "Config/jsmn.h"
#include <sys/time.h>

#include "Audio/processing_parameters.h"
#include "Pipeline/config_loader.h"
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <stdint.h>
#define CC_SHA1_DIGEST_LENGTH 20
typedef uint32_t CC_LONG;

#define SHA1_ROL(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

static void sha1_transform(uint32_t state[5], const unsigned char buffer[64]) {
  uint32_t block[80];
  for (int i = 0; i < 16; i++) {
    block[i] =
        ((uint32_t)buffer[i * 4] << 24) | ((uint32_t)buffer[i * 4 + 1] << 16) |
        ((uint32_t)buffer[i * 4 + 2] << 8) | ((uint32_t)buffer[i * 4 + 3]);
  }
  for (int i = 16; i < 80; i++) {
    block[i] = SHA1_ROL(
        block[i - 3] ^ block[i - 8] ^ block[i - 14] ^ block[i - 16], 1);
  }
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    uint32_t temp = SHA1_ROL(a, 5) + f + e + k + block[i];
    e = d;
    d = c;
    c = SHA1_ROL(b, 30);
    b = a;
    a = temp;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

static void CC_SHA1(const void* data, CC_LONG len, unsigned char* digest) {
  uint32_t state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476,
                       0xC3D2C1F0};
  unsigned char buffer[64];
  uint64_t total_bits = (uint64_t)len * 8;
  const unsigned char* d = (const unsigned char*)data;
  CC_LONG offset = 0;
  CC_LONG remaining_len = len;
  while (remaining_len >= 64) {
    sha1_transform(state, d + offset);
    offset += 64;
    remaining_len -= 64;
  }
  memcpy(buffer, d + offset, remaining_len);
  buffer[remaining_len] = 0x80;
  if (remaining_len >= 56) {
    memset(buffer + remaining_len + 1, 0, 63 - remaining_len);
    sha1_transform(state, buffer);
    memset(buffer, 0, 56);
  } else {
    memset(buffer + remaining_len + 1, 0, 55 - remaining_len);
  }
  for (int i = 0; i < 8; i++) {
    buffer[56 + i] = (unsigned char)(total_bits >> ((7 - i) * 8));
  }
  sha1_transform(state, buffer);
  for (int i = 0; i < 5; i++) {
    digest[i * 4] = (unsigned char)(state[i] >> 24);
    digest[i * 4 + 1] = (unsigned char)(state[i] >> 16);
    digest[i * 4 + 2] = (unsigned char)(state[i] >> 8);
    digest[i * 4 + 3] = (unsigned char)(state[i]);
  }
}
#endif
#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static double db_to_amplitude(double db) {
  if (db <= -1000.0) return 0.0;
  return pow(10.0, db / 20.0);
}

static double amplitude_to_db(double amp) {
  if (amp <= 0.0) return -1000.0;
  double db = 20.0 * log10(amp);
  return db < -1000.0 ? -1000.0 : db;
}

static void level_history_append(level_history_t* history, const double* levels,
                                 size_t channels, uint64_t now_ms) {
  if (history->channels != channels) {
    for (size_t i = 0; i < 300; i++) {
      if (history->samples[i].levels) {
        free(history->samples[i].levels);
        history->samples[i].levels = NULL;
      }
    }
    history->channels = channels;
    history->head = 0;
    history->size = 0;
  }
  if (channels == 0) return;
  level_sample_t* sample = &history->samples[history->head];
  if (sample->levels) {
    free(sample->levels);
  }
  sample->levels = (double*)malloc(channels * sizeof(double));
  if (sample->levels) {
    memcpy(sample->levels, levels, channels * sizeof(double));
    sample->timestamp_ms = now_ms;
    history->head = (history->head + 1) % 300;
    if (history->size < 300) {
      history->size++;
    }
  }
}

static void level_history_get_max_since(const level_history_t* history,
                                        uint64_t since_ms, double* out_levels) {
  size_t channels = history->channels;
  for (size_t c = 0; c < channels; c++) {
    out_levels[c] = -1000.0;
  }
  if (history->size == 0 || channels == 0) return;
  size_t idx = (history->head + 300 - 1) % 300;
  for (size_t i = 0; i < history->size; i++) {
    const level_sample_t* sample = &history->samples[idx];
    if (sample->timestamp_ms < since_ms) break;
    for (size_t c = 0; c < channels; c++) {
      if (sample->levels[c] > out_levels[c]) {
        out_levels[c] = sample->levels[c];
      }
    }
    idx = (idx + 300 - 1) % 300;
  }
}

static void level_history_get_rms_since(const level_history_t* history,
                                        uint64_t since_ms, double* out_levels) {
  size_t channels = history->channels;
  for (size_t c = 0; c < channels; c++) {
    out_levels[c] = -1000.0;
  }
  if (history->size == 0 || channels == 0) return;
  double* sums = (double*)calloc(channels, sizeof(double));
  size_t count = 0;
  size_t idx = (history->head + 300 - 1) % 300;
  for (size_t i = 0; i < history->size; i++) {
    const level_sample_t* sample = &history->samples[idx];
    if (sample->timestamp_ms < since_ms) break;
    for (size_t c = 0; c < channels; c++) {
      double amp = db_to_amplitude(sample->levels[c]);
      sums[c] += amp * amp;
    }
    count++;
    idx = (idx + 300 - 1) % 300;
  }
  if (count > 0) {
    for (size_t c = 0; c < channels; c++) {
      double mean_square = sums[c] / (double)count;
      out_levels[c] = amplitude_to_db(sqrt(mean_square));
    }
  }
  free(sums);
}

static double smoothing_alpha(double delta_ms, double time_constant_ms) {
  if (time_constant_ms <= 0.0) return 1.0;
  double delta_sec = delta_ms / 1000.0;
  double time_constant_sec = time_constant_ms / 1000.0;
  return 1.0 - exp(-delta_sec / time_constant_sec);
}

active_config_path_t* active_config_path_create(const char* initial_path) {
  active_config_path_t* path =
      (active_config_path_t*)calloc(1, sizeof(active_config_path_t));
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

void active_config_path_free(active_config_path_t* path) { free(path); }

websocket_server_t* websocket_server_create(uint16_t port, const char* host,
                                            active_config_path_t* active_path) {
  websocket_server_t* server =
      (websocket_server_t*)calloc(1, sizeof(websocket_server_t));
  if (!server) return NULL;
  server->port = port;
  if (host && host[0]) {
    strncpy(server->host, host, sizeof(server->host) - 1);
  } else {
    strncpy(server->host, "127.0.0.1", sizeof(server->host) - 1);
  }
  server->active_path = active_path;
  server->server_fd = -1;
  server->update_interval = 100;
  atomic_init(&server->running, false);
  return server;
}

/// Set the DSP engine interface for the WebSocket server to interact with.
void websocket_server_set_engine(websocket_server_t* server,
                                 dsp_engine_interface_t* engine) {
  if (server) {
    server->engine = engine;
    // Fetch initial active configuration asynchronously (in C, handled via
    // engine interface)
  }
}

/// Set the state file path for the WebSocket server.
void websocket_server_set_state_file(websocket_server_t* server,
                                     const char* state_file_path) {
  if (server) {
    if (state_file_path && state_file_path[0]) {
      strncpy(server->state_file_path, state_file_path,
              sizeof(server->state_file_path) - 1);
      server->state_file_path[sizeof(server->state_file_path) - 1] = '\0';
      server->has_state_file_path = true;
    } else {
      server->state_file_path[0] = '\0';
      server->has_state_file_path = false;
    }
  }
}

static uint64_t get_time_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

static void stop_reason_to_string(const processing_stop_reason_t* reason,
                                  char* out, size_t max_len) {
  if (!reason || !out || max_len == 0) return;
  switch (reason->type) {
    case STOP_REASON_NONE:
      snprintf(out, max_len, "\"None\"");
      break;
    case STOP_REASON_DONE:
      snprintf(out, max_len, "\"Done\"");
      break;
    case STOP_REASON_CAPTURE_ERROR:
      snprintf(out, max_len, "\"CaptureError: %s\"", reason->message);
      break;
    case STOP_REASON_PLAYBACK_ERROR:
      snprintf(out, max_len, "\"PlaybackError: %s\"", reason->message);
      break;
    case STOP_REASON_CAPTURE_FORMAT_CHANGE:
      snprintf(out, max_len, "\"CaptureFormatChange(%d)\"",
               reason->format_change_rate);
      break;
    case STOP_REASON_PLAYBACK_FORMAT_CHANGE:
      snprintf(out, max_len, "\"PlaybackFormatChange(%d)\"",
               reason->format_change_rate);
      break;
    case STOP_REASON_UNKNOWN_ERROR:
      snprintf(out, max_len, "\"UnknownError: %s\"", reason->message);
      break;
    default:
      snprintf(out, max_len, "\"None\"");
      break;
  }
}

static void format_state_event_payload(processing_state_t state,
                                       const processing_stop_reason_t* reason,
                                       char* out, size_t max_len) {
  char reason_str[512] = "\"None\"";
  stop_reason_to_string(reason, reason_str, sizeof(reason_str));
  snprintf(out, max_len, "{\"state\":\"%s\",\"stop_reason\":%s}",
           processing_state_to_string(state), reason_str);
}

static const char* get_websocket_error_key(audio_backend_error_type_t type) {
  switch (type) {
    case AUDIO_BACKEND_ERR_CONFIG_PARSE:
      return "ConfigValidationError";
    case AUDIO_BACKEND_ERR_DEVICE_NOT_FOUND:
      return "DeviceNotFoundError";
    case AUDIO_BACKEND_ERR_DEVICE_BUSY:
      return "DeviceBusyError";
    default:
      return "DeviceError";
  }
}

static void json_reply(const char* cmd, const char* res_str,
                       const char* val_str, char* out, size_t max_len) {
  if (val_str && val_str[0]) {
    snprintf(out, max_len, "{\"%s\":{\"result\":%s,\"value\":%s}}", cmd,
             res_str, val_str);
  } else {
    snprintf(out, max_len, "{\"%s\":{\"result\":%s}}", cmd, res_str);
  }
}

static char* server_read_file_to_string(const char* path) {
  FILE* fp = fopen(path, "rb");
  if (!fp) return NULL;
  fseek(fp, 0, SEEK_END);
  long len = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (len < 0) {
    fclose(fp);
    return NULL;
  }
  char* buf = (char*)malloc((size_t)len + 1);
  if (!buf) {
    fclose(fp);
    return NULL;
  }
  size_t read_bytes = fread(buf, 1, (size_t)len, fp);
  buf[read_bytes] = '\0';
  fclose(fp);
  return buf;
}

static void get_tok_string(const char* js, const jsmntok_t* tok, char* dest,
                           size_t dest_len) {
  int len = tok->end - tok->start;
  if (len >= (int)dest_len) len = (int)dest_len - 1;
  memcpy(dest, js + tok->start, len);
  dest[len] = '\0';
}

static void get_tok_string_unescape(const char* js, const jsmntok_t* tok,
                                    char* dest, size_t dest_len) {
  int len = tok->end - tok->start;
  int i = 0;
  int j = 0;
  while (i < len && j < (int)dest_len - 1) {
    if (js[tok->start + i] == '\\' && i + 1 < len) {
      char next = js[tok->start + i + 1];
      if (next == 'n')
        dest[j++] = '\n';
      else if (next == 'r')
        dest[j++] = '\r';
      else if (next == 't')
        dest[j++] = '\t';
      else
        dest[j++] = next;
      i += 2;
    } else {
      dest[j++] = js[tok->start + i];
      i++;
    }
  }
  dest[j] = '\0';
}

static double get_tok_double(const char* js, const jsmntok_t* tok) {
  if (!tok) return 0.0;
  char buf[64];
  get_tok_string(js, tok, buf, sizeof(buf));
  return strtod(buf, NULL);
}

static int get_tok_int(const char* js, const jsmntok_t* tok) {
  if (!tok) return 0;
  char buf[64];
  get_tok_string(js, tok, buf, sizeof(buf));
  return (int)strtol(buf, NULL, 10);
}

static bool get_tok_bool(const char* js, const jsmntok_t* tok) {
  if (!tok) return false;
  int len = tok->end - tok->start;
  if (len == 4 && strncmp(js + tok->start, "true", 4) == 0) return true;
  return false;
}

static int skip_token(const jsmntok_t* tokens, int start_idx) {
  int i = start_idx;
  int remaining = 1;
  while (remaining > 0) {
    int children = tokens[i].size;
    remaining += children - 1;
    i++;
  }
  return i;
}

static int find_object_key(const char* js, const jsmntok_t* tokens, int count,
                           int obj_idx, const char* key) {
  if (obj_idx < 0 || obj_idx >= count || tokens[obj_idx].type != JSMN_OBJECT)
    return -1;
  int size = tokens[obj_idx].size;
  int i = obj_idx + 1;
  for (int k = 0; k < size; k++) {
    if (i >= count) return -1;
    if (tokens[i].type == JSMN_STRING) {
      int len = tokens[i].end - tokens[i].start;
      if (len == (int)strlen(key) &&
          strncmp(js + tokens[i].start, key, len) == 0) {
        return i + 1;
      }
    }
    i = skip_token(tokens, i);
  }
  return -1;
}

static int find_top_level_key(const char* js, const jsmntok_t* tokens,
                              int count, const char* key) {
  if (count <= 0 || tokens[0].type != JSMN_OBJECT) return -1;
  int size = tokens[0].size;
  int i = 1;
  for (int k = 0; k < size; k++) {
    if (i >= count) return -1;
    if (tokens[i].type == JSMN_STRING) {
      int len = tokens[i].end - tokens[i].start;
      if (len == (int)strlen(key) &&
          strncmp(js + tokens[i].start, key, len) == 0) {
        return i + 1;
      }
    }
    i = skip_token(tokens, i);
  }
  return -1;
}

static int get_array_element(const jsmntok_t* tokens, int count, int arr_idx,
                             int element_idx) {
  if (arr_idx < 0 || arr_idx >= count || tokens[arr_idx].type != JSMN_ARRAY)
    return -1;
  int size = tokens[arr_idx].size;
  if (element_idx < 0 || element_idx >= size) return -1;
  int i = arr_idx + 1;
  for (int k = 0; k < element_idx; k++) {
    i = skip_token(tokens, i);
  }
  return i;
}

static char* extract_json_string_value_dyn(const char* json, const char* key) {
  if (!json || !key) return NULL;
  jsmn_parser p;
  jsmn_init(&p);
  int num_tokens = jsmn_parse(&p, json, strlen(json), NULL, 0);
  if (num_tokens <= 0) return NULL;
  jsmntok_t* tokens = malloc(num_tokens * sizeof(jsmntok_t));
  if (!tokens) return NULL;
  jsmn_init(&p);
  int count = jsmn_parse(&p, json, strlen(json), tokens, num_tokens);
  if (count <= 0) {
    free(tokens);
    return NULL;
  }
  char* result = NULL;
  int val_idx = find_top_level_key(json, tokens, count, key);
  if (val_idx != -1 && tokens[val_idx].type == JSMN_STRING) {
    int len = tokens[val_idx].end - tokens[val_idx].start;
    result = malloc(len + 1);
    if (result) {
      get_tok_string_unescape(json, &tokens[val_idx], result, len + 1);
    }
  }
  free(tokens);
  return result;
}

static bool extract_json_string_value(const char* json, const char* key,
                                      char* out_buf, size_t max_len) {
  if (!json || !key || !out_buf || max_len == 0) return false;
  jsmn_parser p;
  jsmn_init(&p);
  int num_tokens = jsmn_parse(&p, json, strlen(json), NULL, 0);
  if (num_tokens <= 0) return false;
  jsmntok_t* tokens = malloc(num_tokens * sizeof(jsmntok_t));
  if (!tokens) return false;
  jsmn_init(&p);
  int count = jsmn_parse(&p, json, strlen(json), tokens, num_tokens);
  if (count <= 0) {
    free(tokens);
    return false;
  }
  bool success = false;
  int val_idx = find_top_level_key(json, tokens, count, key);
  if (val_idx != -1 && tokens[val_idx].type == JSMN_STRING) {
    get_tok_string_unescape(json, &tokens[val_idx], out_buf, max_len);
    success = true;
  }
  free(tokens);
  return success;
}

static bool extract_json_double_value(const char* json, const char* key,
                                      double* out_val) {
  if (!json || !key || !out_val) return false;
  jsmn_parser p;
  jsmn_init(&p);
  int num_tokens = jsmn_parse(&p, json, strlen(json), NULL, 0);
  if (num_tokens <= 0) return false;
  jsmntok_t* tokens = malloc(num_tokens * sizeof(jsmntok_t));
  if (!tokens) return false;
  jsmn_init(&p);
  int count = jsmn_parse(&p, json, strlen(json), tokens, num_tokens);
  if (count <= 0) {
    free(tokens);
    return false;
  }
  bool success = false;
  int val_idx = find_top_level_key(json, tokens, count, key);
  if (val_idx != -1 && tokens[val_idx].type == JSMN_PRIMITIVE) {
    *out_val = get_tok_double(json, &tokens[val_idx]);
    success = true;
  }
  free(tokens);
  return success;
}

static bool extract_json_bool_value(const char* json, const char* key,
                                    bool* out_val) {
  if (!json || !key || !out_val) return false;
  jsmn_parser p;
  jsmn_init(&p);
  int num_tokens = jsmn_parse(&p, json, strlen(json), NULL, 0);
  if (num_tokens <= 0) return false;
  jsmntok_t* tokens = malloc(num_tokens * sizeof(jsmntok_t));
  if (!tokens) return false;
  jsmn_init(&p);
  int count = jsmn_parse(&p, json, strlen(json), tokens, num_tokens);
  if (count <= 0) {
    free(tokens);
    return false;
  }
  bool success = false;
  int val_idx = find_top_level_key(json, tokens, count, key);
  if (val_idx != -1 && tokens[val_idx].type == JSMN_PRIMITIVE) {
    *out_val = get_tok_bool(json, &tokens[val_idx]);
    success = true;
  }
  free(tokens);
  return success;
}

static void format_double_array(const double* arr, size_t count, char* out,
                                size_t max_len) {
  if (count == 0) {
    snprintf(out, max_len, "[]");
    return;
  }
  size_t offset = 0;
  offset += snprintf(out + offset, max_len - offset, "[");
  for (size_t i = 0; i < count; i++) {
    offset += snprintf(out + offset, max_len - offset, "%.17g%s", arr[i],
                       (i + 1 < count) ? "," : "");
  }
  snprintf(out + offset, max_len - offset, "]");
}

static bool server_locate_pointer(const char* json, const char* pointer,
                                  const char** out_start,
                                  const char** out_end) {
  if (!json || !pointer || !out_start || !out_end) return false;

  jsmn_parser p;
  jsmn_init(&p);
  int num_tokens = jsmn_parse(&p, json, strlen(json), NULL, 0);
  if (num_tokens <= 0) return false;
  jsmntok_t* tokens = malloc(num_tokens * sizeof(jsmntok_t));
  if (!tokens) return false;
  jsmn_init(&p);
  int count = jsmn_parse(&p, json, strlen(json), tokens, num_tokens);
  if (count <= 0) {
    free(tokens);
    return false;
  }

  int curr_idx = 0;
  const char* ptr = pointer;
  if (*ptr == '/') ptr++;

  bool success = true;
  while (*ptr) {
    char segment[128];
    size_t seg_len = 0;
    while (*ptr && *ptr != '/' && seg_len < sizeof(segment) - 1) {
      segment[seg_len++] = *ptr++;
    }
    segment[seg_len] = '\0';
    if (*ptr == '/') ptr++;

    if (tokens[curr_idx].type == JSMN_OBJECT) {
      int val_idx = find_object_key(json, tokens, count, curr_idx, segment);
      if (val_idx == -1) {
        success = false;
        break;
      }
      curr_idx = val_idx;
    } else if (tokens[curr_idx].type == JSMN_ARRAY) {
      char* endptr = NULL;
      int idx = (int)strtol(segment, &endptr, 10);
      if (endptr == segment || *endptr != '\0') {
        success = false;
        break;
      }
      int el_idx = get_array_element(tokens, count, curr_idx, idx);
      if (el_idx == -1) {
        success = false;
        break;
      }
      curr_idx = el_idx;
    } else {
      success = false;
      break;
    }
  }

  if (success) {
    *out_start = json + tokens[curr_idx].start;
    *out_end = json + tokens[curr_idx].end;
  }
  free(tokens);
  return success;
}

static bool server_get_value_at_pointer(const char* json, const char* pointer,
                                        char* out_val, size_t max_len) {
  const char* start = NULL;
  const char* end = NULL;
  if (!server_locate_pointer(json, pointer, &start, &end)) return false;
  size_t result_len = (size_t)(end - start);
  if (result_len >= max_len) result_len = max_len - 1;
  memcpy(out_val, start, result_len);
  out_val[result_len] = '\0';
  return true;
}

static char* server_set_value_at_pointer_str(const char* json,
                                             const char* pointer,
                                             const char* new_val_str) {
  const char* start = NULL;
  const char* end = NULL;
  if (!server_locate_pointer(json, pointer, &start, &end)) return NULL;

  size_t prefix_len = (size_t)(start - json);
  size_t suffix_len = strlen(end);
  size_t val_len = strlen(new_val_str);

  char* new_json = (char*)malloc(prefix_len + val_len + suffix_len + 1);
  if (!new_json) return NULL;

  memcpy(new_json, json, prefix_len);
  memcpy(new_json + prefix_len, new_val_str, val_len);
  memcpy(new_json + prefix_len + val_len, end, suffix_len);
  new_json[prefix_len + val_len + suffix_len] = '\0';

  return new_json;
}

static bool server_merge_patch_tokens(char** p_target_json, const char* js,
                                      const jsmntok_t* tokens, int count,
                                      int obj_idx, char* current_path,
                                      size_t path_max) {
  if (tokens[obj_idx].type != JSMN_OBJECT) return false;
  int size = tokens[obj_idx].size;
  int i = obj_idx + 1;
  for (int k = 0; k < size; k++) {
    char key[128];
    get_tok_string_unescape(js, &tokens[i], key, sizeof(key));

    int val_idx = i + 1;

    size_t orig_path_len = strlen(current_path);
    snprintf(current_path + orig_path_len, path_max - orig_path_len, "/%s",
             key);

    if (tokens[val_idx].type == JSMN_OBJECT) {
      if (!server_merge_patch_tokens(p_target_json, js, tokens, count, val_idx,
                                     current_path, path_max)) {
        return false;
      }
    } else {
      int val_len = tokens[val_idx].end - tokens[val_idx].start;
      char* val_str = (char*)malloc(val_len + 1);
      if (val_str) {
        memcpy(val_str, js + tokens[val_idx].start, val_len);
        val_str[val_len] = '\0';

        char* updated = server_set_value_at_pointer_str(*p_target_json,
                                                        current_path, val_str);
        if (updated) {
          free(*p_target_json);
          *p_target_json = updated;
        }
        free(val_str);
      }
    }
    current_path[orig_path_len] = '\0';

    i = skip_token(tokens, i);
  }
  return true;
}

static void format_device_descriptor(const audio_device_descriptor_t* desc,
                                     char* out, size_t max_len) {
  if (!desc) {
    snprintf(out, max_len, "null");
    return;
  }
  size_t offset = 0;
  offset += snprintf(out + offset, max_len - offset,
                     "{\"name\":\"%s\",\"capability_sets\":[", desc->name);
  for (size_t cs_idx = 0; cs_idx < desc->capability_sets_count; cs_idx++) {
    const device_capability_set_t* cs = &desc->capability_sets[cs_idx];
    offset += snprintf(out + offset, max_len - offset, "{\"capabilities\":[");
    for (size_t c_idx = 0; c_idx < cs->capabilities_count; c_idx++) {
      const channel_capability_t* cap = &cs->capabilities[c_idx];
      offset += snprintf(out + offset, max_len - offset,
                         "{\"channels\":%d,\"samplerates\":[", cap->channels);
      for (size_t s_idx = 0; s_idx < cap->samplerates_count; s_idx++) {
        const samplerate_capability_t* sr = &cap->samplerates[s_idx];
        offset += snprintf(out + offset, max_len - offset,
                           "{\"samplerate\":%d,\"formats\":[", sr->samplerate);
        for (size_t f_idx = 0; f_idx < sr->formats_count; f_idx++) {
          offset += snprintf(out + offset, max_len - offset, "\"%s\"%s",
                             sr->formats[f_idx],
                             (f_idx + 1 < sr->formats_count) ? "," : "");
        }
        offset += snprintf(out + offset, max_len - offset, "]}%s",
                           (s_idx + 1 < cap->samplerates_count) ? "," : "");
      }
      offset += snprintf(out + offset, max_len - offset, "]}%s",
                         (c_idx + 1 < cs->capabilities_count) ? "," : "");
    }
    offset += snprintf(out + offset, max_len - offset, "]}%s",
                       (cs_idx + 1 < desc->capability_sets_count) ? "," : "");
  }
  snprintf(out + offset, max_len - offset, "]}");
}

static void format_spectrum(const spectrum_t* spec, char* out, size_t max_len) {
  if (!spec || spec->count == 0) {
    snprintf(out, max_len, "null");
    return;
  }
  size_t offset = 0;
  offset += snprintf(out + offset, max_len - offset, "{\"frequencies\":[");
  for (size_t i = 0; i < spec->count; i++) {
    offset += snprintf(out + offset, max_len - offset, "%.17g%s",
                       spec->frequencies[i], (i + 1 < spec->count) ? "," : "");
  }
  offset += snprintf(out + offset, max_len - offset, "],\"magnitudes\":[");
  for (size_t i = 0; i < spec->count; i++) {
    offset += snprintf(out + offset, max_len - offset, "%.17g%s",
                       spec->magnitudes[i], (i + 1 < spec->count) ? "," : "");
  }
  snprintf(out + offset, max_len - offset, "]}");
}

static bool server_handle_adjust_volume_fader(
    websocket_server_t* server, fader_t fader, const char* arg_start,
    char* out_response, size_t max_len, const char* cmd_name) {
  processing_parameters_t* params = NULL;
  if (!server || !server->engine ||
      !server->engine->get_processing_parameters ||
      !server->engine->get_processing_parameters(server->engine->ctx,
                                                 (void**)&params) ||
      !params) {
    json_reply(cmd_name, "\"ProcessingNotRunningError\"", NULL, out_response,
               max_len);
    return true;
  }

  double delta = 0.0;
  double min_vol = -150.0;
  double max_vol = 50.0;

  while (*arg_start && (*arg_start == ' ' || *arg_start == '\t')) arg_start++;
  if (*arg_start == '[') {
    arg_start++;
    char* endptr = NULL;
    delta = strtod(arg_start, &endptr);
    if (endptr != arg_start) {
      arg_start = endptr;
      while (*arg_start &&
             (*arg_start == ' ' || *arg_start == ',' || *arg_start == '\t'))
        arg_start++;
      min_vol = strtod(arg_start, &endptr);
      if (endptr != arg_start) {
        arg_start = endptr;
        while (*arg_start &&
               (*arg_start == ' ' || *arg_start == ',' || *arg_start == '\t'))
          arg_start++;
        max_vol = strtod(arg_start, &endptr);
      }
    }
  } else {
    char* endptr = NULL;
    delta = strtod(arg_start, &endptr);
    if (endptr == arg_start) {
      return false;
    }
  }

  double current =
      processing_parameters_get_target_volume_for_fader(params, fader);
  double new_vol = current + delta;
  if (new_vol < min_vol) new_vol = min_vol;
  if (new_vol > max_vol) new_vol = max_vol;

  processing_parameters_set_target_volume_for_fader(params, new_vol, fader);
  server->unsaved_state_changes = true;

  char val[64];
  if (fader == FADER_MAIN) {
    snprintf(val, sizeof(val), "%.17g", new_vol);
  } else {
    snprintf(val, sizeof(val), "[%d,%.17g]", (int)fader, new_vol);
  }
  json_reply(cmd_name, "\"Ok\"", val, out_response, max_len);
  return true;
}

// MARK: - Command Handler

/// Handle a control command text (either simple quoted string or JSON object)
/// and populate out_response.
void websocket_server_handle_command(websocket_server_t* server, int client_idx,
                                     const char* command_text,
                                     char* out_response, size_t max_len) {
  if (!out_response || max_len == 0) return;
  out_response[0] = '\0';
  if (!command_text) return;

  jsmntok_t local_tokens[128];
  jsmntok_t* tokens = local_tokens;

  jsmn_parser parser;
  jsmn_init(&parser);
  int num_tokens =
      jsmn_parse(&parser, command_text, strlen(command_text), NULL, 0);
  if (num_tokens <= 0) {
    json_reply("Invalid", "{\"error\":\"Invalid JSON\"}", NULL, out_response,
               max_len);
    return;
  }

  if (num_tokens > 128) {
    tokens = malloc(num_tokens * sizeof(jsmntok_t));
    if (!tokens) return;
  }

  jsmn_init(&parser);
  int count = jsmn_parse(&parser, command_text, strlen(command_text), tokens,
                         num_tokens);
  if (count <= 0) {
    if (tokens != local_tokens) free(tokens);
    json_reply("Invalid", "{\"error\":\"Invalid JSON\"}", NULL, out_response,
               max_len);
    return;
  }

  char cmd_name[128] = "";
  int arg_idx = -1;
  if (tokens[0].type == JSMN_STRING) {
    get_tok_string_unescape(command_text, &tokens[0], cmd_name,
                            sizeof(cmd_name));
  } else if (tokens[0].type == JSMN_OBJECT) {
    if (tokens[0].size > 0 && tokens[1].type == JSMN_STRING) {
      get_tok_string_unescape(command_text, &tokens[1], cmd_name,
                              sizeof(cmd_name));
      arg_idx = 2;
    }
  }

  const char* simple = cmd_name;

  if (strcmp(simple, "GetVersion") == 0) {
    json_reply("GetVersion", "\"Ok\"", "\"CamillaDSP-C-Embedded 2.0.0\"",
               out_response, max_len);
  } else if (strcmp(simple, "GetState") == 0) {
    processing_state_t state = PROCESSING_STATE_INACTIVE;
    if (server && server->engine && server->engine->get_status) {
      state_update_t status;
      if (server->engine->get_status(server->engine->ctx, &status)) {
        state = status.state;
      }
    }
    char val[64];
    snprintf(val, sizeof(val), "\"%s\"", processing_state_to_string(state));
    json_reply("GetState", "\"Ok\"", val, out_response, max_len);
  } else if (strcmp(simple, "GetStopReason") == 0) {
    char reason_str[512] = "\"None\"";
    if (server && server->engine && server->engine->get_status) {
      state_update_t status;
      if (server->engine->get_status(server->engine->ctx, &status)) {
        stop_reason_to_string(&status.stop_reason, reason_str,
                              sizeof(reason_str));
      }
    }
    json_reply("GetStopReason", "\"Ok\"", reason_str, out_response, max_len);
  } else if (strcmp(simple, "GetVolume") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      double vol =
          processing_parameters_get_target_volume_for_fader(params, FADER_MAIN);
      char val[64];
      snprintf(val, sizeof(val), "%.17g", vol);
      json_reply("GetVolume", "\"Ok\"", val, out_response, max_len);
    } else {
      json_reply("GetVolume", "\"ProcessingNotRunningError\"", NULL,
                 out_response, max_len);
    }
  } else if (strcmp(simple, "GetMute") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      bool muted = processing_parameters_is_muted_for_fader(params, FADER_MAIN);
      json_reply("GetMute", "\"Ok\"", muted ? "true" : "false", out_response,
                 max_len);
    } else {
      json_reply("GetMute", "\"ProcessingNotRunningError\"", NULL, out_response,
                 max_len);
    }
  } else if (strcmp(simple, "ToggleMute") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      bool was_muted =
          processing_parameters_is_muted_for_fader(params, FADER_MAIN);
      processing_parameters_set_muted_for_fader(params, !was_muted, FADER_MAIN);
      if (server) server->unsaved_state_changes = true;
      json_reply("ToggleMute", "\"Ok\"", !was_muted ? "true" : "false",
                 out_response, max_len);
    } else {
      json_reply("ToggleMute", "\"ProcessingNotRunningError\"", NULL,
                 out_response, max_len);
    }
  } else if (strcmp(simple, "GetFaders") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      char faders_val[1024];
      int offset = 0;
      offset += snprintf(faders_val + offset, sizeof(faders_val) - offset, "[");
      for (int i = 0; i < FADER_COUNT; i++) {
        double vol = processing_parameters_get_target_volume_for_fader(
            params, (fader_t)i);
        bool muted =
            processing_parameters_is_muted_for_fader(params, (fader_t)i);
        offset += snprintf(faders_val + offset, sizeof(faders_val) - offset,
                           "{\"volume\":%.17g,\"mute\":%s}%s", vol,
                           muted ? "true" : "false",
                           (i < FADER_COUNT - 1) ? "," : "");
      }
      snprintf(faders_val + offset, sizeof(faders_val) - offset, "]");
      json_reply("GetFaders", "\"Ok\"", faders_val, out_response, max_len);
    } else {
      json_reply("GetFaders", "\"ProcessingNotRunningError\"", NULL,
                 out_response, max_len);
    }
  } else if (strcmp(simple, "GetCaptureSignalRms") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      size_t count = params->capture_channels;
      double* levels = (double*)calloc(count, sizeof(double));
      if (levels) {
        processing_parameters_get_capture_signal_rms(params, levels, count);
        char val[1024];
        format_double_array(levels, count, val, sizeof(val));
        free(levels);
        json_reply("GetCaptureSignalRms", "\"Ok\"", val, out_response, max_len);
      } else {
        json_reply("GetCaptureSignalRms", "\"Ok\"", "[]", out_response,
                   max_len);
      }
    } else {
      json_reply("GetCaptureSignalRms", "\"ProcessingNotRunningError\"", NULL,
                 out_response, max_len);
    }
  } else if (strcmp(simple, "GetCaptureSignalPeak") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      size_t count = params->capture_channels;
      double* levels = (double*)calloc(count, sizeof(double));
      if (levels) {
        processing_parameters_get_capture_signal_peak(params, levels, count);
        char val[1024];
        format_double_array(levels, count, val, sizeof(val));
        free(levels);
        json_reply("GetCaptureSignalPeak", "\"Ok\"", val, out_response,
                   max_len);
      } else {
        json_reply("GetCaptureSignalPeak", "\"Ok\"", "[]", out_response,
                   max_len);
      }
    } else {
      json_reply("GetCaptureSignalPeak", "\"ProcessingNotRunningError\"", NULL,
                 out_response, max_len);
    }
  } else if (strcmp(simple, "GetPlaybackSignalRms") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      size_t count = params->playback_channels;
      double* levels = (double*)calloc(count, sizeof(double));
      if (levels) {
        processing_parameters_get_playback_signal_rms(params, levels, count);
        char val[1024];
        format_double_array(levels, count, val, sizeof(val));
        free(levels);
        json_reply("GetPlaybackSignalRms", "\"Ok\"", val, out_response,
                   max_len);
      } else {
        json_reply("GetPlaybackSignalRms", "\"Ok\"", "[]", out_response,
                   max_len);
      }
    } else {
      json_reply("GetPlaybackSignalRms", "\"ProcessingNotRunningError\"", NULL,
                 out_response, max_len);
    }
  } else if (strcmp(simple, "GetPlaybackSignalPeak") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      size_t count = params->playback_channels;
      double* levels = (double*)calloc(count, sizeof(double));
      if (levels) {
        processing_parameters_get_playback_signal_peak(params, levels, count);
        char val[1024];
        format_double_array(levels, count, val, sizeof(val));
        free(levels);
        json_reply("GetPlaybackSignalPeak", "\"Ok\"", val, out_response,
                   max_len);
      } else {
        json_reply("GetPlaybackSignalPeak", "\"Ok\"", "[]", out_response,
                   max_len);
      }
    } else {
      json_reply("GetPlaybackSignalPeak", "\"ProcessingNotRunningError\"", NULL,
                 out_response, max_len);
    }
  } else if (strcmp(simple, "GetCaptureRate") == 0) {
    state_update_t status;
    memset(&status, 0, sizeof(status));
    bool has_status = server && server->engine && server->engine->get_status &&
                      server->engine->get_status(server->engine->ctx, &status);
    if (has_status && status.state == PROCESSING_STATE_RUNNING) {
      const dsp_config_t* config =
          (server && server->engine && server->engine->get_active_config)
              ? server->engine->get_active_config(server->engine->ctx)
              : NULL;
      int sr = config ? config->devices.samplerate : 0;
      char val[32];
      snprintf(val, sizeof(val), "%d", sr);
      json_reply("GetCaptureRate", "\"Ok\"", val, out_response, max_len);
    } else {
      json_reply("GetCaptureRate", "\"Ok\"", "0", out_response, max_len);
    }
  } else if (strcmp(simple, "GetRateAdjust") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      double rate = atomic_double_get(&params->rate_adjust);
      char val[32];
      snprintf(val, sizeof(val), "%.17g", rate);
      json_reply("GetRateAdjust", "\"Ok\"", val, out_response, max_len);
    } else {
      json_reply("GetRateAdjust", "\"Ok\"", "1.0", out_response, max_len);
    }
  } else if (strcmp(simple, "GetBufferLevel") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      double lvl = atomic_double_get(&params->buffer_level);
      char val[32];
      snprintf(val, sizeof(val), "%d", (int)lvl);
      json_reply("GetBufferLevel", "\"Ok\"", val, out_response, max_len);
    } else {
      json_reply("GetBufferLevel", "\"Ok\"", "0", out_response, max_len);
    }
  } else if (strcmp(simple, "GetClippedSamples") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      uint64_t clips =
          atomic_load_explicit(&params->clipped_samples, memory_order_relaxed);
      char val[32];
      snprintf(val, sizeof(val), "%llu", (unsigned long long)clips);
      json_reply("GetClippedSamples", "\"Ok\"", val, out_response, max_len);
    } else {
      json_reply("GetClippedSamples", "\"Ok\"", "0", out_response, max_len);
    }
  } else if (strcmp(simple, "ResetClippedSamples") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      atomic_store_explicit(&params->clipped_samples, 0ULL,
                            memory_order_relaxed);
    }
    json_reply("ResetClippedSamples", "\"Ok\"", NULL, out_response, max_len);
  } else if (strcmp(simple, "GetProcessingLoad") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      double load = atomic_double_get(&params->processing_load);
      char val[32];
      snprintf(val, sizeof(val), "%.17g", load);
      json_reply("GetProcessingLoad", "\"Ok\"", val, out_response, max_len);
    } else {
      json_reply("GetProcessingLoad", "\"Ok\"", "0.0", out_response, max_len);
    }
  } else if (strcmp(simple, "GetResamplerLoad") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      double load = atomic_double_get(&params->resampler_load);
      char val[32];
      snprintf(val, sizeof(val), "%.17g", load);
      json_reply("GetResamplerLoad", "\"Ok\"", val, out_response, max_len);
    } else {
      json_reply("GetResamplerLoad", "\"Ok\"", "0.0", out_response, max_len);
    }
  } else if (strcmp(simple, "GetSupportedDeviceTypes") == 0) {
    json_reply("GetSupportedDeviceTypes", "\"Ok\"",
               "[[\"CoreAudio\"],[\"CoreAudio\"]]", out_response, max_len);
  } else if (strcmp(simple, "GetUpdateInterval") == 0) {
    char val[32];
    snprintf(val, sizeof(val), "%d", server ? server->update_interval : 100);
    json_reply("GetUpdateInterval", "\"Ok\"", val, out_response, max_len);
  } else if (strstr(command_text, "\"SetUpdateInterval\"")) {
    double val;
    if (extract_json_double_value(command_text, "SetUpdateInterval", &val)) {
      if (val >= 0.0) {
        if (server) server->update_interval = (uint32_t)val;
        json_reply("SetUpdateInterval", "\"Ok\"", NULL, out_response, max_len);
      } else {
        json_reply("SetUpdateInterval",
                   "{\"InvalidValueError\":\"Value must be >= 0\"}", NULL,
                   out_response, max_len);
      }
    } else {
      json_reply("SetUpdateInterval",
                 "{\"InvalidRequestError\":\"Could not parse SetUpdateInterval "
                 "argument\"}",
                 NULL, out_response, max_len);
    }
  } else if (strcmp(simple, "SubscribeState") == 0) {
    if (server) {
      server->client_sessions[client_idx].state_subscribed = true;
    }
    json_reply("SubscribeState", "\"Ok\"", NULL, out_response, max_len);
  } else if (strstr(command_text, "\"SubscribeVuLevels\"") &&
             strcmp(simple, "SubscribeVuLevels") != 0) {
    double max_rate = 0;
    double attack = 0;
    double release = 0;
    extract_json_double_value(command_text, "max_rate", &max_rate);
    extract_json_double_value(command_text, "attack", &attack);
    extract_json_double_value(command_text, "release", &release);
    if (attack < 0.0 || attack > 60000.0 || release < 0.0 ||
        release > 60000.0) {
      json_reply("SubscribeVuLevels",
                 "{\"InvalidValueError\":\"attack and release must be between "
                 "0 and 60000 ms\"}",
                 NULL, out_response, max_len);
    } else {
      if (server) {
        server->client_sessions[client_idx].vu_subscribed = true;
        server->client_sessions[client_idx].vu_max_rate = max_rate;
        server->client_sessions[client_idx].vu_attack = attack;
        server->client_sessions[client_idx].vu_release = release;
        server->client_sessions[client_idx].last_vu_push_time = 0;
      }
      json_reply("SubscribeVuLevels", "\"Ok\"", NULL, out_response, max_len);
    }
  } else if (strstr(command_text, "\"SubscribeSignalLevels\"")) {
    char side[16] = "";
    if (extract_json_string_value(command_text, "SubscribeSignalLevels", side,
                                  sizeof(side))) {
      if (strcmp(side, "playback") == 0 || strcmp(side, "capture") == 0 ||
          strcmp(side, "both") == 0) {
        if (server) {
          server->client_sessions[client_idx].signal_levels_subscribed = true;
          snprintf(
              server->client_sessions[client_idx].signal_levels_side,
              sizeof(server->client_sessions[client_idx].signal_levels_side),
              "%s", side);
        }
        json_reply("SubscribeSignalLevels", "\"Ok\"", NULL, out_response,
                   max_len);
      } else {
        json_reply("SubscribeSignalLevels",
                   "{\"InvalidValueError\":\"side must be playback, capture, "
                   "or both\"}",
                   NULL, out_response, max_len);
      }
    } else {
      json_reply("SubscribeSignalLevels",
                 "{\"InvalidRequestError\":\"Could not parse side argument\"}",
                 NULL, out_response, max_len);
    }
  } else if (strstr(command_text, "\"SubscribeSpectrum\"")) {
    bool is_capture = true;
    uint32_t channel = 0;
    double min_freq = 20.0;
    double max_freq = 20000.0;
    uint32_t n_bins = 1024;
    double max_rate = 0.0;
    bool ok = false;

    if (arg_idx != -1 && tokens[arg_idx].type == JSMN_OBJECT) {
      int ic_idx =
          find_object_key(command_text, tokens, count, arg_idx, "is_capture");
      if (ic_idx != -1)
        is_capture = get_tok_bool(command_text, &tokens[ic_idx]);
      int ch_idx =
          find_object_key(command_text, tokens, count, arg_idx, "channel");
      if (ch_idx != -1)
        channel = (uint32_t)get_tok_int(command_text, &tokens[ch_idx]);
      int mn_idx =
          find_object_key(command_text, tokens, count, arg_idx, "min_freq");
      if (mn_idx != -1)
        min_freq = get_tok_double(command_text, &tokens[mn_idx]);
      int mx_idx =
          find_object_key(command_text, tokens, count, arg_idx, "max_freq");
      if (mx_idx != -1)
        max_freq = get_tok_double(command_text, &tokens[mx_idx]);
      int nb_idx =
          find_object_key(command_text, tokens, count, arg_idx, "n_bins");
      if (nb_idx != -1)
        n_bins = (uint32_t)get_tok_int(command_text, &tokens[nb_idx]);
      int mr_idx =
          find_object_key(command_text, tokens, count, arg_idx, "max_rate");
      if (mr_idx != -1)
        max_rate = get_tok_double(command_text, &tokens[mr_idx]);
      ok = true;
    }

    if (ok) {
      if (server) {
        server->client_sessions[client_idx].spectrum_subscribed = true;
        server->client_sessions[client_idx].spectrum_is_capture = is_capture;
        server->client_sessions[client_idx].spectrum_channel = channel;
        server->client_sessions[client_idx].spectrum_min_freq = min_freq;
        server->client_sessions[client_idx].spectrum_max_freq = max_freq;
        server->client_sessions[client_idx].spectrum_n_bins = n_bins;
        server->client_sessions[client_idx].spectrum_max_rate = max_rate;
        server->client_sessions[client_idx].last_spectrum_push_time = 0;
      }
      json_reply("SubscribeSpectrum", "\"Ok\"", NULL, out_response, max_len);
    } else {
      json_reply(
          "SubscribeSpectrum",
          "{\"InvalidRequestError\":\"Could not parse SubscribeSpectrum arguments\"}",
          NULL, out_response, max_len);
    }
  } else if (strcmp(simple, "StopSubscription") == 0) {
    if (server) {
      bool active =
          server->client_sessions[client_idx].state_subscribed ||
          server->client_sessions[client_idx].vu_subscribed ||
          server->client_sessions[client_idx].signal_levels_subscribed ||
          server->client_sessions[client_idx].spectrum_subscribed;
      if (active) {
        server->client_sessions[client_idx].state_subscribed = false;
        server->client_sessions[client_idx].vu_subscribed = false;
        server->client_sessions[client_idx].signal_levels_subscribed = false;
        server->client_sessions[client_idx].spectrum_subscribed = false;
        json_reply("StopSubscription", "\"Ok\"", NULL, out_response, max_len);
      } else {
        json_reply("StopSubscription",
                   "{\"InvalidRequestError\":\"No active subscription\"}", NULL,
                   out_response, max_len);
      }
    } else {
      json_reply("StopSubscription",
                 "{\"InvalidRequestError\":\"No active subscription\"}", NULL,
                 out_response, max_len);
    }
  } else if (strcmp(simple, "GetCaptureSignalRmsSinceLast") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      uint64_t since = server->client_sessions[client_idx].last_cap_rms_time;
      server->client_sessions[client_idx].last_cap_rms_time = get_time_ms();
      size_t ch = server->capture_rms_history.channels;
      double* rms = (double*)calloc(ch, sizeof(double));
      level_history_get_rms_since(&server->capture_rms_history, since, rms);
      char* rms_str = (char*)malloc(ch * 30 + 10);
      format_double_array(rms, ch, rms_str, ch * 30 + 10);
      json_reply("GetCaptureSignalRmsSinceLast", "\"Ok\"", rms_str,
                 out_response, max_len);
      free(rms_str);
      free(rms);
    } else {
      json_reply("GetCaptureSignalRmsSinceLast",
                 "\"ProcessingNotRunningError\"", NULL, out_response, max_len);
    }
  } else if (strcmp(simple, "GetCaptureSignalPeakSinceLast") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      uint64_t since = server->client_sessions[client_idx].last_cap_peak_time;
      server->client_sessions[client_idx].last_cap_peak_time = get_time_ms();
      size_t ch = server->capture_peak_history.channels;
      double* pk = (double*)calloc(ch, sizeof(double));
      level_history_get_max_since(&server->capture_peak_history, since, pk);
      char* pk_str = (char*)malloc(ch * 30 + 10);
      format_double_array(pk, ch, pk_str, ch * 30 + 10);
      json_reply("GetCaptureSignalPeakSinceLast", "\"Ok\"", pk_str,
                 out_response, max_len);
      free(pk_str);
      free(pk);
    } else {
      json_reply("GetCaptureSignalPeakSinceLast",
                 "\"ProcessingNotRunningError\"", NULL, out_response, max_len);
    }
  } else if (strcmp(simple, "GetPlaybackSignalRmsSinceLast") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      uint64_t since = server->client_sessions[client_idx].last_pb_rms_time;
      server->client_sessions[client_idx].last_pb_rms_time = get_time_ms();
      size_t ch = server->playback_rms_history.channels;
      double* rms = (double*)calloc(ch, sizeof(double));
      level_history_get_rms_since(&server->playback_rms_history, since, rms);
      char* rms_str = (char*)malloc(ch * 30 + 10);
      format_double_array(rms, ch, rms_str, ch * 30 + 10);
      json_reply("GetPlaybackSignalRmsSinceLast", "\"Ok\"", rms_str,
                 out_response, max_len);
      free(rms_str);
      free(rms);
    } else {
      json_reply("GetPlaybackSignalRmsSinceLast",
                 "\"ProcessingNotRunningError\"", NULL, out_response, max_len);
    }
  } else if (strcmp(simple, "GetPlaybackSignalPeakSinceLast") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      uint64_t since = server->client_sessions[client_idx].last_pb_peak_time;
      server->client_sessions[client_idx].last_pb_peak_time = get_time_ms();
      size_t ch = server->playback_peak_history.channels;
      double* pk = (double*)calloc(ch, sizeof(double));
      level_history_get_max_since(&server->playback_peak_history, since, pk);
      char* pk_str = (char*)malloc(ch * 30 + 10);
      format_double_array(pk, ch, pk_str, ch * 30 + 10);
      json_reply("GetPlaybackSignalPeakSinceLast", "\"Ok\"", pk_str,
                 out_response, max_len);
      free(pk_str);
      free(pk);
    } else {
      json_reply("GetPlaybackSignalPeakSinceLast",
                 "\"ProcessingNotRunningError\"", NULL, out_response, max_len);
    }
  } else if (strstr(command_text, "\"GetCaptureSignalRmsSince\"")) {
    double secs = 0;
    if (extract_json_double_value(command_text, "GetCaptureSignalRmsSince",
                                  &secs)) {
      processing_parameters_t* params = NULL;
      if (server && server->engine &&
          server->engine->get_processing_parameters &&
          server->engine->get_processing_parameters(server->engine->ctx,
                                                    (void**)&params) &&
          params) {
        uint64_t now = get_time_ms();
        uint64_t since = now - (uint64_t)(secs * 1000.0);
        size_t ch = server->capture_rms_history.channels;
        double* rms = (double*)calloc(ch, sizeof(double));
        level_history_get_rms_since(&server->capture_rms_history, since, rms);
        char* rms_str = (char*)malloc(ch * 30 + 10);
        format_double_array(rms, ch, rms_str, ch * 30 + 10);
        json_reply("GetCaptureSignalRmsSince", "\"Ok\"", rms_str, out_response,
                   max_len);
        free(rms_str);
        free(rms);
      } else {
        json_reply("GetCaptureSignalRmsSince", "\"ProcessingNotRunningError\"",
                   NULL, out_response, max_len);
      }
    } else {
      json_reply("GetCaptureSignalRmsSince",
                 "{\"InvalidRequestError\":\"Could not parse seconds\"}", NULL,
                 out_response, max_len);
    }
  } else if (strstr(command_text, "\"GetCaptureSignalPeakSince\"")) {
    double secs = 0;
    if (extract_json_double_value(command_text, "GetCaptureSignalPeakSince",
                                  &secs)) {
      processing_parameters_t* params = NULL;
      if (server && server->engine &&
          server->engine->get_processing_parameters &&
          server->engine->get_processing_parameters(server->engine->ctx,
                                                    (void**)&params) &&
          params) {
        uint64_t now = get_time_ms();
        uint64_t since = now - (uint64_t)(secs * 1000.0);
        size_t ch = server->capture_peak_history.channels;
        double* pk = (double*)calloc(ch, sizeof(double));
        level_history_get_max_since(&server->capture_peak_history, since, pk);
        char* pk_str = (char*)malloc(ch * 30 + 10);
        format_double_array(pk, ch, pk_str, ch * 30 + 10);
        json_reply("GetCaptureSignalPeakSince", "\"Ok\"", pk_str, out_response,
                   max_len);
        free(pk_str);
        free(pk);
      } else {
        json_reply("GetCaptureSignalPeakSince", "\"ProcessingNotRunningError\"",
                   NULL, out_response, max_len);
      }
    } else {
      json_reply("GetCaptureSignalPeakSince",
                 "{\"InvalidRequestError\":\"Could not parse seconds\"}", NULL,
                 out_response, max_len);
    }
  } else if (strstr(command_text, "\"GetPlaybackSignalRmsSince\"")) {
    double secs = 0;
    if (extract_json_double_value(command_text, "GetPlaybackSignalRmsSince",
                                  &secs)) {
      processing_parameters_t* params = NULL;
      if (server && server->engine &&
          server->engine->get_processing_parameters &&
          server->engine->get_processing_parameters(server->engine->ctx,
                                                    (void**)&params) &&
          params) {
        uint64_t now = get_time_ms();
        uint64_t since = now - (uint64_t)(secs * 1000.0);
        size_t ch = server->playback_rms_history.channels;
        double* rms = (double*)calloc(ch, sizeof(double));
        level_history_get_rms_since(&server->playback_rms_history, since, rms);
        char* rms_str = (char*)malloc(ch * 30 + 10);
        format_double_array(rms, ch, rms_str, ch * 30 + 10);
        json_reply("GetPlaybackSignalRmsSince", "\"Ok\"", rms_str, out_response,
                   max_len);
        free(rms_str);
        free(rms);
      } else {
        json_reply("GetPlaybackSignalRmsSince", "\"ProcessingNotRunningError\"",
                   NULL, out_response, max_len);
      }
    } else {
      json_reply("GetPlaybackSignalRmsSince",
                 "{\"InvalidRequestError\":\"Could not parse seconds\"}", NULL,
                 out_response, max_len);
    }
  } else if (strstr(command_text, "\"GetPlaybackSignalPeakSince\"")) {
    double secs = 0;
    if (extract_json_double_value(command_text, "GetPlaybackSignalPeakSince",
                                  &secs)) {
      processing_parameters_t* params = NULL;
      if (server && server->engine &&
          server->engine->get_processing_parameters &&
          server->engine->get_processing_parameters(server->engine->ctx,
                                                    (void**)&params) &&
          params) {
        uint64_t now = get_time_ms();
        uint64_t since = now - (uint64_t)(secs * 1000.0);
        size_t ch = server->playback_peak_history.channels;
        double* pk = (double*)calloc(ch, sizeof(double));
        level_history_get_max_since(&server->playback_peak_history, since, pk);
        char* pk_str = (char*)malloc(ch * 30 + 10);
        format_double_array(pk, ch, pk_str, ch * 30 + 10);
        json_reply("GetPlaybackSignalPeakSince", "\"Ok\"", pk_str, out_response,
                   max_len);
        free(pk_str);
        free(pk);
      } else {
        json_reply("GetPlaybackSignalPeakSince",
                   "\"ProcessingNotRunningError\"", NULL, out_response,
                   max_len);
      }
    } else {
      json_reply("GetPlaybackSignalPeakSince",
                 "{\"InvalidRequestError\":\"Could not parse seconds\"}", NULL,
                 out_response, max_len);
    }
  } else if (strcmp(simple, "GetSignalLevels") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      size_t p_ch = params->playback_channels;
      size_t c_ch = params->capture_channels;
      double* p_rms = (double*)calloc(p_ch, sizeof(double));
      double* p_pk = (double*)calloc(p_ch, sizeof(double));
      double* c_rms = (double*)calloc(c_ch, sizeof(double));
      double* c_pk = (double*)calloc(c_ch, sizeof(double));
      if (p_rms && p_pk && c_rms && c_pk) {
        processing_parameters_get_playback_signal_rms(params, p_rms, p_ch);
        processing_parameters_get_playback_signal_peak(params, p_pk, p_ch);
        processing_parameters_get_capture_signal_rms(params, c_rms, c_ch);
        processing_parameters_get_capture_signal_peak(params, c_pk, c_ch);
        char* p_rms_str = (char*)malloc(p_ch * 30 + 10);
        char* p_pk_str = (char*)malloc(p_ch * 30 + 10);
        char* c_rms_str = (char*)malloc(c_ch * 30 + 10);
        char* c_pk_str = (char*)malloc(c_ch * 30 + 10);
        if (p_rms_str && p_pk_str && c_rms_str && c_pk_str) {
          format_double_array(p_rms, p_ch, p_rms_str, p_ch * 30 + 10);
          format_double_array(p_pk, p_ch, p_pk_str, p_ch * 30 + 10);
          format_double_array(c_rms, c_ch, c_rms_str, c_ch * 30 + 10);
          format_double_array(c_pk, c_ch, c_pk_str, c_ch * 30 + 10);
          char* val = (char*)malloc((p_ch + c_ch) * 120 + 200);
          if (val) {
            sprintf(val,
                    "{\"playback_rms\":%s,\"playback_peak\":%s,\"capture_rms\":"
                    "%s,\"capture_peak\":%s}",
                    p_rms_str, p_pk_str, c_rms_str, c_pk_str);
            json_reply("GetSignalLevels", "\"Ok\"", val, out_response, max_len);
            free(val);
          }
        }
        if (p_rms_str) free(p_rms_str);
        if (p_pk_str) free(p_pk_str);
        if (c_rms_str) free(c_rms_str);
        if (c_pk_str) free(c_pk_str);
      }
      if (p_rms) free(p_rms);
      if (p_pk) free(p_pk);
      if (c_rms) free(c_rms);
      if (c_pk) free(c_pk);
    } else {
      json_reply("GetSignalLevels", "\"ProcessingNotRunningError\"", NULL,
                 out_response, max_len);
    }
  } else if (strcmp(simple, "GetSignalLevelsSinceLast") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      uint64_t cap_rms_since =
          server->client_sessions[client_idx].last_cap_rms_time;
      uint64_t cap_pk_since =
          server->client_sessions[client_idx].last_cap_peak_time;
      uint64_t pb_rms_since =
          server->client_sessions[client_idx].last_pb_rms_time;
      uint64_t pb_pk_since =
          server->client_sessions[client_idx].last_pb_peak_time;
      uint64_t now = get_time_ms();
      server->client_sessions[client_idx].last_cap_rms_time = now;
      server->client_sessions[client_idx].last_cap_peak_time = now;
      server->client_sessions[client_idx].last_pb_rms_time = now;
      server->client_sessions[client_idx].last_pb_peak_time = now;
      size_t c_ch = server->capture_rms_history.channels;
      size_t p_ch = server->playback_rms_history.channels;
      double* c_rms = (double*)calloc(c_ch, sizeof(double));
      double* c_pk = (double*)calloc(c_ch, sizeof(double));
      double* p_rms = (double*)calloc(p_ch, sizeof(double));
      double* p_pk = (double*)calloc(p_ch, sizeof(double));
      level_history_get_rms_since(&server->capture_rms_history, cap_rms_since,
                                  c_rms);
      level_history_get_max_since(&server->capture_peak_history, cap_pk_since,
                                  c_pk);
      level_history_get_rms_since(&server->playback_rms_history, pb_rms_since,
                                  p_rms);
      level_history_get_max_since(&server->playback_peak_history, pb_pk_since,
                                  p_pk);
      char* c_rms_str = (char*)malloc(c_ch * 30 + 10);
      char* c_pk_str = (char*)malloc(c_ch * 30 + 10);
      char* p_rms_str = (char*)malloc(p_ch * 30 + 10);
      char* p_pk_str = (char*)malloc(p_ch * 30 + 10);
      format_double_array(c_rms, c_ch, c_rms_str, c_ch * 30 + 10);
      format_double_array(c_pk, c_ch, c_pk_str, c_ch * 30 + 10);
      format_double_array(p_rms, p_ch, p_rms_str, p_ch * 30 + 10);
      format_double_array(p_pk, p_ch, p_pk_str, p_ch * 30 + 10);
      char* val = (char*)malloc((c_ch + p_ch) * 120 + 200);
      sprintf(val,
              "{\"playback_rms\":%s,\"playback_peak\":%s,\"capture_rms\":%s,"
              "\"capture_peak\":%s}",
              p_rms_str, p_pk_str, c_rms_str, c_pk_str);
      json_reply("GetSignalLevelsSinceLast", "\"Ok\"", val, out_response,
                 max_len);
      free(val);
      free(c_rms_str);
      free(c_pk_str);
      free(p_rms_str);
      free(p_pk_str);
      free(c_rms);
      free(c_pk);
      free(p_rms);
      free(p_pk);
    } else {
      json_reply("GetSignalLevelsSinceLast", "\"ProcessingNotRunningError\"",
                 NULL, out_response, max_len);
    }
  } else if (strstr(command_text, "\"GetSignalLevelsSince\"")) {
    double secs = 0;
    if (extract_json_double_value(command_text, "GetSignalLevelsSince",
                                  &secs)) {
      processing_parameters_t* params = NULL;
      if (server && server->engine &&
          server->engine->get_processing_parameters &&
          server->engine->get_processing_parameters(server->engine->ctx,
                                                    (void**)&params) &&
          params) {
        uint64_t now = get_time_ms();
        uint64_t since = now - (uint64_t)(secs * 1000.0);
        size_t c_ch = server->capture_rms_history.channels;
        size_t p_ch = server->playback_rms_history.channels;
        double* c_rms = (double*)calloc(c_ch, sizeof(double));
        double* c_pk = (double*)calloc(c_ch, sizeof(double));
        double* p_rms = (double*)calloc(p_ch, sizeof(double));
        double* p_pk = (double*)calloc(p_ch, sizeof(double));
        level_history_get_rms_since(&server->capture_rms_history, since, c_rms);
        level_history_get_max_since(&server->capture_peak_history, since, c_pk);
        level_history_get_rms_since(&server->playback_rms_history, since,
                                    p_rms);
        level_history_get_max_since(&server->playback_peak_history, since,
                                    p_pk);
        char* c_rms_str = (char*)malloc(c_ch * 30 + 10);
        char* c_pk_str = (char*)malloc(c_ch * 30 + 10);
        char* p_rms_str = (char*)malloc(p_ch * 30 + 10);
        char* p_pk_str = (char*)malloc(p_ch * 30 + 10);
        format_double_array(c_rms, c_ch, c_rms_str, c_ch * 30 + 10);
        format_double_array(c_pk, c_ch, c_pk_str, c_ch * 30 + 10);
        format_double_array(p_rms, p_ch, p_rms_str, p_ch * 30 + 10);
        format_double_array(p_pk, p_ch, p_pk_str, p_ch * 30 + 10);
        char* val = (char*)malloc((c_ch + p_ch) * 120 + 200);
        sprintf(val,
                "{\"playback_rms\":%s,\"playback_peak\":%s,\"capture_rms\":%s,"
                "\"capture_peak\":%s}",
                p_rms_str, p_pk_str, c_rms_str, c_pk_str);
        json_reply("GetSignalLevelsSince", "\"Ok\"", val, out_response,
                   max_len);
        free(val);
        free(c_rms_str);
        free(c_pk_str);
        free(p_rms_str);
        free(p_pk_str);
        free(c_rms);
        free(c_pk);
        free(p_rms);
        free(p_pk);
      } else {
        json_reply("GetSignalLevelsSince", "\"ProcessingNotRunningError\"",
                   NULL, out_response, max_len);
      }
    } else {
      json_reply("GetSignalLevelsSince",
                 "{\"InvalidRequestError\":\"Could not parse seconds\"}", NULL,
                 out_response, max_len);
    }
  } else if (strcmp(simple, "GetSignalPeaksSinceStart") == 0) {
    char val[2048];
    int offset = 0;
    offset += snprintf(val + offset, sizeof(val) - offset, "{\"capture\":[");
    for (size_t i = 0; i < server->capture_global_peaks_count; i++) {
      offset +=
          snprintf(val + offset, sizeof(val) - offset, "%.17g%s",
                   server->capture_global_peaks[i],
                   (i + 1 < server->capture_global_peaks_count) ? "," : "");
    }
    offset += snprintf(val + offset, sizeof(val) - offset, "],\"playback\":[");
    for (size_t i = 0; i < server->playback_global_peaks_count; i++) {
      offset +=
          snprintf(val + offset, sizeof(val) - offset, "%.17g%s",
                   server->playback_global_peaks[i],
                   (i + 1 < server->playback_global_peaks_count) ? "," : "");
    }
    snprintf(val + offset, sizeof(val) - offset, "]}");
    json_reply("GetSignalPeaksSinceStart", "\"Ok\"", val, out_response,
               max_len);
  } else if (strcmp(simple, "ResetSignalPeaksSinceStart") == 0) {
    for (size_t i = 0; i < server->capture_global_peaks_count; i++) {
      server->capture_global_peaks[i] = -1000.0;
    }
    for (size_t i = 0; i < server->playback_global_peaks_count; i++) {
      server->playback_global_peaks[i] = -1000.0;
    }
    json_reply("ResetSignalPeaksSinceStart", "\"Ok\"", NULL, out_response,
               max_len);
  } else if (strcmp(simple, "GetChannelLabels") == 0) {
    char* json = NULL;
    if (server && server->active_config_json) {
      json = strdup(server->active_config_json);
    } else if (server && server->active_path &&
               server->active_path->has_value) {
      json = server_read_file_to_string(server->active_path->path);
    }
    char play_labels[2048] = "null";
    char cap_labels[2048] = "null";
    if (json) {
      server_get_value_at_pointer(json, "/devices/playback/labels",
                                  play_labels, sizeof(play_labels));
      server_get_value_at_pointer(json, "/devices/capture/labels",
                                  cap_labels, sizeof(cap_labels));
    }
    char val[4096];
    snprintf(val, sizeof(val), "{\"playback\":%s,\"capture\":%s}", play_labels,
             cap_labels);
    json_reply("GetChannelLabels", "\"Ok\"", val, out_response, max_len);
    if (json) free(json);
  } else if (strcmp(simple, "GetSignalRange") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      size_t count = params->playback_channels;
      double max_peak = -1000.0;
      for (size_t i = 0; i < count; i++) {
        double pk = atomic_double_get(&params->playback_signal_peak[i]);
        if (pk > max_peak) max_peak = pk;
      }
      double range = 2.0 * db_to_amplitude(max_peak);
      char val[64];
      snprintf(val, sizeof(val), "%.17g", range);
      json_reply("GetSignalRange", "\"Ok\"", val, out_response, max_len);
    } else {
      json_reply("GetSignalRange", "\"ProcessingNotRunningError\"", NULL,
                 out_response, max_len);
    }
  } else if (strcmp(simple, "GetConfigFilePath") == 0) {
    const char* path = server && server->active_path
                           ? active_config_path_get(server->active_path)
                           : NULL;
    char val[1100];
    if (path)
      snprintf(val, sizeof(val), "\"%s\"", path);
    else
      snprintf(val, sizeof(val), "null");
    json_reply("GetConfigFilePath", "\"Ok\"", val, out_response, max_len);
  } else if (strcmp(simple, "GetPreviousConfig") == 0) {
    const char* prev = server ? server->previous_config_json : NULL;
    char val[1100];
    if (prev)
      snprintf(val, sizeof(val), "\"%s\"", prev);
    else
      snprintf(val, sizeof(val), "null");
    json_reply("GetPreviousConfig", "\"Ok\"", val, out_response, max_len);
  } else if (strcmp(simple, "GetStateFilePath") == 0) {
    const char* path =
        server && server->has_state_file_path ? server->state_file_path : NULL;
    char val[1100];
    if (path)
      snprintf(val, sizeof(val), "\"%s\"", path);
    else
      snprintf(val, sizeof(val), "null");
    json_reply("GetStateFilePath", "\"Ok\"", val, out_response, max_len);
  } else if (strcmp(simple, "GetStateFileUpdated") == 0) {
    bool updated = server ? !server->unsaved_state_changes : true;
    json_reply("GetStateFileUpdated", "\"Ok\"", updated ? "true" : "false",
               out_response, max_len);
  } else if (strcmp(simple, "GetConfig") == 0 ||
             strcmp(simple, "GetConfigJson") == 0) {
    char* json = NULL;
    if (server && server->active_config_json) {
      json = strdup(server->active_config_json);
    } else if (server && server->active_path &&
               server->active_path->has_value) {
      json = server_read_file_to_string(server->active_path->path);
      if (json) {
        server->active_config_json = strdup(json);
      }
    }
    if (json) {
      json_reply(simple, "\"Ok\"", json, out_response, max_len);
      free(json);
    } else {
      json_reply(simple, "{\"InvalidRequestError\":\"No active config\"}", NULL,
                 out_response, max_len);
    }
  } else if (strcmp(simple, "GetConfigTitle") == 0) {
    char* json = NULL;
    if (server && server->active_config_json) {
      json = strdup(server->active_config_json);
    } else if (server && server->active_path &&
               server->active_path->has_value) {
      json = server_read_file_to_string(server->active_path->path);
    }
    char title[256];
    if (json &&
        extract_json_string_value(json, "title", title, sizeof(title))) {
      char val[300];
      snprintf(val, sizeof(val), "\"%s\"", title);
      json_reply("GetConfigTitle", "\"Ok\"", val, out_response, max_len);
    } else {
      json_reply("GetConfigTitle", "\"Ok\"", "null", out_response, max_len);
    }
    if (json) free(json);
  } else if (strcmp(simple, "GetConfigDescription") == 0) {
    char* json = NULL;
    if (server && server->active_config_json) {
      json = strdup(server->active_config_json);
    } else if (server && server->active_path &&
               server->active_path->has_value) {
      json = server_read_file_to_string(server->active_path->path);
    }
    char desc[512];
    if (json &&
        extract_json_string_value(json, "description", desc, sizeof(desc))) {
      char val[600];
      snprintf(val, sizeof(val), "\"%s\"", desc);
      json_reply("GetConfigDescription", "\"Ok\"", val, out_response, max_len);
    } else {
      json_reply("GetConfigDescription", "\"Ok\"", "null", out_response,
                 max_len);
    }
    if (json) free(json);
  } else if (strcmp(simple, "Reload") == 0) {
    const char* path =
        (server && server->active_path && server->active_path->has_value)
            ? server->active_path->path
            : NULL;
    if (path) {
      char* json = server_read_file_to_string(path);
      if (json) {
        audio_backend_error_t err;
        memset(&err, 0, sizeof(err));
        bool ok = server && server->engine && server->engine->set_config_json &&
                  server->engine->set_config_json(server->engine->ctx, json, &err);
        if (ok) {
          if (server->previous_config_json) free(server->previous_config_json);
          server->previous_config_json = server->active_config_json;
          server->active_config_json = strdup(json);
          if (server) server->unsaved_state_changes = false;
          json_reply("Reload", "\"Ok\"", NULL, out_response, max_len);
        } else {
          char val[600];
          snprintf(val, sizeof(val), "{\"%s\":\"%s\"}",
                   get_websocket_error_key(err.type), err.message);
          json_reply("Reload", val, NULL, out_response, max_len);
        }
        free(json);
      } else {
        json_reply("Reload",
                   "{\"ConfigReadError\":\"Could not read config file\"}", NULL,
                   out_response, max_len);
      }
    } else {
      json_reply("Reload",
                 "{\"InvalidRequestError\":\"No config file path set\"}", NULL,
                 out_response, max_len);
    }
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
  } else if (strstr(command_text, "\"SetVolume\"")) {
    double vol;
    if (extract_json_double_value(command_text, "SetVolume", &vol)) {
      processing_parameters_t* params = NULL;
      if (server && server->engine &&
          server->engine->get_processing_parameters &&
          server->engine->get_processing_parameters(server->engine->ctx,
                                                    (void**)&params) &&
          params) {
        double clamped = vol < -150.0 ? -150.0 : (vol > 50.0 ? 50.0 : vol);
        processing_parameters_set_target_volume_for_fader(params, clamped,
                                                          FADER_MAIN);
        if (server) server->unsaved_state_changes = true;
        json_reply("SetVolume", "\"Ok\"", NULL, out_response, max_len);
      } else {
        json_reply("SetVolume", "\"ProcessingNotRunningError\"", NULL,
                   out_response, max_len);
      }
    } else {
      json_reply("SetVolume",
                 "{\"InvalidRequestError\":\"Could not parse volume value\"}",
                 NULL, out_response, max_len);
    }
  } else if (strstr(command_text, "\"SetMute\"")) {
    bool mute;
    if (extract_json_bool_value(command_text, "SetMute", &mute)) {
      processing_parameters_t* params = NULL;
      if (server && server->engine &&
          server->engine->get_processing_parameters &&
          server->engine->get_processing_parameters(server->engine->ctx,
                                                    (void**)&params) &&
          params) {
        processing_parameters_set_muted_for_fader(params, mute, FADER_MAIN);
        if (server) server->unsaved_state_changes = true;
        json_reply("SetMute", "\"Ok\"", NULL, out_response, max_len);
      } else {
        json_reply("SetMute", "\"ProcessingNotRunningError\"", NULL,
                   out_response, max_len);
      }
    } else {
      json_reply("SetMute",
                 "{\"InvalidRequestError\":\"Could not parse mute value\"}",
                 NULL, out_response, max_len);
    }
  } else if (strstr(command_text, "\"SetConfigFilePath\"")) {
    char* path =
        extract_json_string_value_dyn(command_text, "SetConfigFilePath");
    if (path) {
      if (server && server->active_path) {
        active_config_path_set(server->active_path, path);
        if (server->active_config_json) {
          free(server->active_config_json);
          server->active_config_json = NULL;
        }
      }
      free(path);
      json_reply("SetConfigFilePath", "\"Ok\"", NULL, out_response, max_len);
    } else {
      json_reply(
          "SetConfigFilePath",
          "{\"InvalidRequestError\":\"Could not parse Config File Path\"}",
          NULL, out_response, max_len);
    }
  } else if (strstr(command_text, "\"SetConfigJson\"")) {
    char* new_json =
        extract_json_string_value_dyn(command_text, "SetConfigJson");
    if (new_json) {
      audio_backend_error_t err;
      memset(&err, 0, sizeof(err));
      bool ok = server && server->engine && server->engine->set_config_json &&
                server->engine->set_config_json(server->engine->ctx, new_json, &err);
      if (ok) {
        if (server->previous_config_json) free(server->previous_config_json);
        server->previous_config_json = server->active_config_json;
        server->active_config_json = strdup(new_json);
        if (server) server->unsaved_state_changes = false;
        json_reply("SetConfigJson", "\"Ok\"", NULL, out_response, max_len);
      } else {
        char val[600];
        snprintf(val, sizeof(val), "{\"%s\":\"%s\"}",
                 get_websocket_error_key(err.type), err.message);
        json_reply("SetConfigJson", val, NULL, out_response, max_len);
      }
      free(new_json);
    } else {
      json_reply("SetConfigJson",
                 "{\"InvalidRequestError\":\"Could not parse Config JSON\"}",
                 NULL, out_response, max_len);
    }
  } else if (strstr(command_text, "\"GetConfigValue\"")) {
    char* pointer =
        extract_json_string_value_dyn(command_text, "GetConfigValue");
    if (pointer) {
      char* json = NULL;
      if (server && server->active_config_json) {
        json = strdup(server->active_config_json);
      } else if (server && server->active_path &&
                 server->active_path->has_value) {
        json = server_read_file_to_string(server->active_path->path);
      }

      char val[2048];
      if (json &&
          server_get_value_at_pointer(json, pointer, val, sizeof(val))) {
        json_reply("GetConfigValue", "\"Ok\"", val, out_response, max_len);
      } else {
        char err[256];
        snprintf(err, sizeof(err),
                 "{\"InvalidRequestError\":\"Path not found: %s\"}", pointer);
        json_reply("GetConfigValue", err, NULL, out_response, max_len);
      }
      if (json) free(json);
      free(pointer);
    } else {
      json_reply("GetConfigValue",
                 "{\"InvalidRequestError\":\"Could not parse pointer\"}", NULL,
                 out_response, max_len);
    }
  } else if (strcmp(simple, "SetConfigValue") == 0) {
    char pointer[256] = "";
    int val_idx = -1;
    if (arg_idx != -1 && tokens[arg_idx].type == JSMN_OBJECT) {
      int p_key =
          find_object_key(command_text, tokens, count, arg_idx, "pointer");
      if (p_key != -1) {
        get_tok_string_unescape(command_text, &tokens[p_key], pointer,
                                sizeof(pointer));
        val_idx =
            find_object_key(command_text, tokens, count, arg_idx, "value");
      } else {
        if (tokens[arg_idx].size > 0) {
          get_tok_string_unescape(command_text, &tokens[arg_idx + 1], pointer,
                                  sizeof(pointer));
          val_idx = arg_idx + 2;
        }
      }
    }

    if (pointer[0] != '\0' && val_idx != -1) {
      int v_len = tokens[val_idx].end - tokens[val_idx].start;
      char* trimmed_val = malloc(v_len + 1);
      if (trimmed_val) {
        memcpy(trimmed_val, command_text + tokens[val_idx].start, v_len);
        trimmed_val[v_len] = '\0';

        char* active_json = NULL;
        if (server && server->active_config_json) {
          active_json = strdup(server->active_config_json);
        } else if (server && server->active_path &&
                   server->active_path->has_value) {
          active_json = server_read_file_to_string(server->active_path->path);
        }

        if (active_json) {
          char* updated_json = server_set_value_at_pointer_str(
              active_json, pointer, trimmed_val);
          if (updated_json) {
            audio_backend_error_t err;
            memset(&err, 0, sizeof(err));
            bool ok = server && server->engine &&
                      server->engine->set_config_json &&
                      server->engine->set_config_json(server->engine->ctx,
                                                      updated_json, &err);
            if (ok) {
              if (server->previous_config_json)
                free(server->previous_config_json);
              server->previous_config_json = server->active_config_json;
              server->active_config_json = strdup(updated_json);
              if (server) server->unsaved_state_changes = true;
              json_reply("SetConfigValue", "\"Ok\"", NULL, out_response,
                         max_len);
            } else {
              char val[600];
              snprintf(val, sizeof(val), "{\"%s\":\"%s\"}",
                       get_websocket_error_key(err.type), err.message);
              json_reply("SetConfigValue", val, NULL, out_response, max_len);
            }
            free(updated_json);
          } else {
            char err[256];
            snprintf(err, sizeof(err),
                     "{\"InvalidRequestError\":\"Path not found: %s\"}",
                     pointer);
            json_reply("SetConfigValue", err, NULL, out_response, max_len);
          }
          free(active_json);
        } else {
          json_reply("SetConfigValue",
                     "{\"InvalidRequestError\":\"No active config to modify\"}",
                     NULL, out_response, max_len);
        }
        free(trimmed_val);
      }
    } else {
      json_reply("SetConfigValue",
                 "{\"InvalidRequestError\":\"Could not parse SetConfigValue "
                 "command\"}",
                 NULL, out_response, max_len);
    }
  } else if (strcmp(simple, "PatchConfig") == 0) {
    if (arg_idx != -1 && tokens[arg_idx].type == JSMN_OBJECT) {
      char* active_json = NULL;
      if (server && server->active_config_json) {
        active_json = strdup(server->active_config_json);
      } else if (server && server->active_path &&
                 server->active_path->has_value) {
        active_json = server_read_file_to_string(server->active_path->path);
      }

      if (active_json) {
        char path_buf[512] = {0};
        char* target_json = strdup(active_json);
        if (server_merge_patch_tokens(&target_json, command_text, tokens, count,
                                      arg_idx, path_buf, sizeof(path_buf))) {
          audio_backend_error_t err;
          memset(&err, 0, sizeof(err));
          bool ok =
              server && server->engine && server->engine->set_config_json &&
              server->engine->set_config_json(server->engine->ctx, target_json, &err);
          if (ok) {
            if (server->previous_config_json)
              free(server->previous_config_json);
            server->previous_config_json = server->active_config_json;
            server->active_config_json = strdup(target_json);
            if (server) server->unsaved_state_changes = true;
            json_reply("PatchConfig", "\"Ok\"", NULL, out_response, max_len);
          } else {
            char val[600];
            snprintf(val, sizeof(val), "{\"%s\":\"%s\"}",
                     get_websocket_error_key(err.type), err.message);
            json_reply("PatchConfig", val, NULL, out_response, max_len);
          }
        } else {
          json_reply("PatchConfig",
                     "{\"InvalidRequestError\":\"Failed to merge patch JSON\"}",
                     NULL, out_response, max_len);
        }
        free(target_json);
        free(active_json);
      } else {
        json_reply("PatchConfig",
                   "{\"InvalidRequestError\":\"No active config to patch\"}",
                   NULL, out_response, max_len);
      }
    } else {
      json_reply(
          "PatchConfig",
          "{\"InvalidRequestError\":\"Could not parse PatchConfig command\"}",
          NULL, out_response, max_len);
    }
  } else if (strstr(command_text, "\"GetFaderVolume\"")) {
    double idx_val;
    if (extract_json_double_value(command_text, "GetFaderVolume", &idx_val)) {
      int idx = (int)idx_val;
      processing_parameters_t* params = NULL;
      if (server && server->engine &&
          server->engine->get_processing_parameters &&
          server->engine->get_processing_parameters(server->engine->ctx,
                                                    (void**)&params) &&
          params) {
        if (idx >= 0 && idx < FADER_COUNT) {
          double vol = processing_parameters_get_target_volume_for_fader(
              params, (fader_t)idx);
          char val[64];
          snprintf(val, sizeof(val), "[%d,%.17g]", idx, vol);
          json_reply("GetFaderVolume", "\"Ok\"", val, out_response, max_len);
        } else {
          json_reply("GetFaderVolume", "\"InvalidFaderError\"", NULL,
                     out_response, max_len);
        }
      } else {
        json_reply("GetFaderVolume", "\"ProcessingNotRunningError\"", NULL,
                   out_response, max_len);
      }
    } else {
      json_reply("GetFaderVolume",
                 "{\"InvalidRequestError\":\"Could not parse fader index\"}",
                 NULL, out_response, max_len);
    }
  } else if (strcmp(simple, "SetFaderVolume") == 0 ||
             strcmp(simple, "SetFaderExternalVolume") == 0) {
    int idx = -1;
    double vol = 0.0;
    bool ok = false;
    if (arg_idx != -1 && tokens[arg_idx].type == JSMN_ARRAY &&
        tokens[arg_idx].size >= 2) {
      int idx_tok = get_array_element(tokens, count, arg_idx, 0);
      int val_tok = get_array_element(tokens, count, arg_idx, 1);
      if (idx_tok != -1 && val_tok != -1) {
        idx = get_tok_int(command_text, &tokens[idx_tok]);
        vol = get_tok_double(command_text, &tokens[val_tok]);
        ok = true;
      }
    }
    if (ok) {
      processing_parameters_t* params = NULL;
      if (server && server->engine &&
          server->engine->get_processing_parameters &&
          server->engine->get_processing_parameters(server->engine->ctx,
                                                    (void**)&params) &&
          params) {
        if (idx >= 0 && idx < FADER_COUNT) {
          double clamped = vol < -150.0 ? -150.0 : (vol > 50.0 ? 50.0 : vol);
          processing_parameters_set_target_volume_for_fader(params, clamped,
                                                            (fader_t)idx);
          if (strcmp(simple, "SetFaderExternalVolume") == 0) {
            processing_parameters_set_current_volume_for_fader(params, clamped,
                                                               (fader_t)idx);
          }
          if (server) server->unsaved_state_changes = true;
          json_reply(simple, "\"Ok\"", NULL, out_response, max_len);
        } else {
          json_reply(simple, "\"InvalidFaderError\"", NULL, out_response,
                     max_len);
        }
      } else {
        json_reply(simple, "\"ProcessingNotRunningError\"", NULL, out_response,
                   max_len);
      }
    } else {
      json_reply(simple,
                 "{\"InvalidRequestError\":\"Could not parse "
                 "SetFaderVolume/SetFaderExternalVolume array\"}",
                 NULL, out_response, max_len);
    }
  } else if (strcmp(simple, "GetFaderMute") == 0) {
    double idx_val;
    if (extract_json_double_value(command_text, "GetFaderMute", &idx_val)) {
      int idx = (int)idx_val;
      processing_parameters_t* params = NULL;
      if (server && server->engine &&
          server->engine->get_processing_parameters &&
          server->engine->get_processing_parameters(server->engine->ctx,
                                                    (void**)&params) &&
          params) {
        if (idx >= 0 && idx < FADER_COUNT) {
          bool muted =
              processing_parameters_is_muted_for_fader(params, (fader_t)idx);
          char val[64];
          snprintf(val, sizeof(val), "[%d,%s]", idx, muted ? "true" : "false");
          json_reply("GetFaderMute", "\"Ok\"", val, out_response, max_len);
        } else {
          json_reply("GetFaderMute", "\"InvalidFaderError\"", NULL,
                     out_response, max_len);
        }
      } else {
        json_reply("GetFaderMute", "\"ProcessingNotRunningError\"", NULL,
                   out_response, max_len);
      }
    } else {
      json_reply("GetFaderMute",
                 "{\"InvalidRequestError\":\"Could not parse fader index\"}",
                 NULL, out_response, max_len);
    }
  } else if (strcmp(simple, "SetFaderMute") == 0) {
    int idx = -1;
    bool mute = false;
    bool ok = false;
    if (arg_idx != -1 && tokens[arg_idx].type == JSMN_ARRAY &&
        tokens[arg_idx].size >= 2) {
      int idx_tok = get_array_element(tokens, count, arg_idx, 0);
      int val_tok = get_array_element(tokens, count, arg_idx, 1);
      if (idx_tok != -1 && val_tok != -1) {
        idx = get_tok_int(command_text, &tokens[idx_tok]);
        mute = get_tok_bool(command_text, &tokens[val_tok]);
        ok = true;
      }
    }
    if (ok) {
      processing_parameters_t* params = NULL;
      if (server && server->engine &&
          server->engine->get_processing_parameters &&
          server->engine->get_processing_parameters(server->engine->ctx,
                                                    (void**)&params) &&
          params) {
        if (idx >= 0 && idx < FADER_COUNT) {
          processing_parameters_set_muted_for_fader(params, mute, (fader_t)idx);
          if (server) server->unsaved_state_changes = true;
          json_reply("SetFaderMute", "\"Ok\"", NULL, out_response, max_len);
        } else {
          json_reply("SetFaderMute", "\"InvalidFaderError\"", NULL,
                     out_response, max_len);
        }
      } else {
        json_reply("SetFaderMute", "\"ProcessingNotRunningError\"", NULL,
                   out_response, max_len);
      }
    } else {
      json_reply(
          "SetFaderMute",
          "{\"InvalidRequestError\":\"Could not parse SetFaderMute array\"}",
          NULL, out_response, max_len);
    }
  } else if (strstr(command_text, "\"ToggleFaderMute\"")) {
    double idx_val;
    if (extract_json_double_value(command_text, "ToggleFaderMute", &idx_val)) {
      int idx = (int)idx_val;
      processing_parameters_t* params = NULL;
      if (server && server->engine &&
          server->engine->get_processing_parameters &&
          server->engine->get_processing_parameters(server->engine->ctx,
                                                    (void**)&params) &&
          params) {
        if (idx >= 0 && idx < FADER_COUNT) {
          bool was_muted =
              processing_parameters_is_muted_for_fader(params, (fader_t)idx);
          processing_parameters_set_muted_for_fader(params, !was_muted,
                                                    (fader_t)idx);
          if (server) server->unsaved_state_changes = true;
          char val[64];
          snprintf(val, sizeof(val), "[%d,%s]", idx,
                   !was_muted ? "true" : "false");
          json_reply("ToggleFaderMute", "\"Ok\"", val, out_response, max_len);
        } else {
          json_reply("ToggleFaderMute", "\"InvalidFaderError\"", NULL,
                     out_response, max_len);
        }
      } else {
        json_reply("ToggleFaderMute", "\"ProcessingNotRunningError\"", NULL,
                   out_response, max_len);
      }
    } else {
      json_reply("ToggleFaderMute",
                 "{\"InvalidRequestError\":\"Could not parse fader index\"}",
                 NULL, out_response, max_len);
    }
  } else if (strstr(command_text, "\"GetAvailableCaptureDevices\"")) {
    char* backend = extract_json_string_value_dyn(command_text,
                                                  "GetAvailableCaptureDevices");
    if (backend) {
      audio_device_t* devs = NULL;
      size_t count = 0;
      bool ok = server && server->engine &&
                server->engine->get_available_devices &&
                server->engine->get_available_devices(
                    server->engine->ctx, backend, true, &devs, &count);
      if (ok && devs) {
        char val[4096];
        int offset = 0;
        offset += snprintf(val + offset, sizeof(val) - offset, "[");
        for (size_t i = 0; i < count; i++) {
          offset += snprintf(val + offset, sizeof(val) - offset, "\"%s\"%s",
                             devs[i].name, (i + 1 < count) ? "," : "");
        }
        snprintf(val + offset, sizeof(val) - offset, "]");
        json_reply("GetAvailableCaptureDevices", "\"Ok\"", val, out_response,
                   max_len);
      } else {
        json_reply("GetAvailableCaptureDevices", "\"Ok\"", "[]", out_response,
                   max_len);
      }
      free(backend);
    } else {
      json_reply("GetAvailableCaptureDevices",
                 "{\"InvalidRequestError\":\"Could not parse backend\"}", NULL,
                 out_response, max_len);
    }
  } else if (strstr(command_text, "\"GetAvailablePlaybackDevices\"")) {
    char* backend = extract_json_string_value_dyn(
        command_text, "GetAvailablePlaybackDevices");
    if (backend) {
      audio_device_t* devs = NULL;
      size_t count = 0;
      bool ok = server && server->engine &&
                server->engine->get_available_devices &&
                server->engine->get_available_devices(
                    server->engine->ctx, backend, false, &devs, &count);
      if (ok && devs) {
        char val[4096];
        int offset = 0;
        offset += snprintf(val + offset, sizeof(val) - offset, "[");
        for (size_t i = 0; i < count; i++) {
          offset += snprintf(val + offset, sizeof(val) - offset, "\"%s\"%s",
                             devs[i].name, (i + 1 < count) ? "," : "");
        }
        snprintf(val + offset, sizeof(val) - offset, "]");
        json_reply("GetAvailablePlaybackDevices", "\"Ok\"", val, out_response,
                   max_len);
      } else {
        json_reply("GetAvailablePlaybackDevices", "\"Ok\"", "[]", out_response,
                   max_len);
      }
      free(backend);
    } else {
      json_reply("GetAvailablePlaybackDevices",
                 "{\"InvalidRequestError\":\"Could not parse backend\"}", NULL,
                 out_response, max_len);
    }
  } else if (strstr(command_text, "\"AdjustVolume\"")) {
    const char* pos = strstr(command_text, "\"AdjustVolume\"");
    if (pos) {
      pos += 14;
      while (*pos && *pos != ':') pos++;
      if (*pos == ':') {
        pos++;
        if (server_handle_adjust_volume_fader(server, FADER_MAIN, pos,
                                              out_response, max_len,
                                              "AdjustVolume")) {
          goto adjust_volume_done;
        }
      }
    }
    json_reply(
        "AdjustVolume",
        "{\"InvalidRequestError\":\"Could not parse AdjustVolume argument\"}",
        NULL, out_response, max_len);
  adjust_volume_done:;
  } else if (strstr(command_text, "\"AdjustFaderVolume\"")) {
    const char* pos = strstr(command_text, "\"AdjustFaderVolume\"");
    if (pos) {
      pos += 19;
      while (*pos && *pos != '[') pos++;
      if (*pos == '[') {
        pos++;
        char* endptr = NULL;
        int idx = (int)strtol(pos, &endptr, 10);
        if (endptr != pos) {
          pos = endptr;
          while (*pos && (*pos == ' ' || *pos == ',' || *pos == '\t')) pos++;
          if (idx >= 0 && idx < FADER_COUNT) {
            if (server_handle_adjust_volume_fader(server, (fader_t)idx, pos,
                                                  out_response, max_len,
                                                  "AdjustFaderVolume")) {
              goto adjust_fader_volume_done;
            }
          } else {
            json_reply("AdjustFaderVolume", "\"InvalidFaderError\"", NULL,
                       out_response, max_len);
            goto adjust_fader_volume_done;
          }
        }
      }
    }
    json_reply(
        "AdjustFaderVolume",
        "{\"InvalidRequestError\":\"Could not parse AdjustFaderVolume array\"}",
        NULL, out_response, max_len);
  adjust_fader_volume_done:;
  } else if (strcmp(simple, "GetCaptureDeviceCapabilities") == 0 ||
             strcmp(simple, "GetPlaybackDeviceCapabilities") == 0) {
    char backend[128] = "";
    char device[256] = "";
    bool ok = false;
    if (arg_idx != -1 && tokens[arg_idx].type == JSMN_ARRAY &&
        tokens[arg_idx].size >= 2) {
      int b_tok = get_array_element(tokens, count, arg_idx, 0);
      int d_tok = get_array_element(tokens, count, arg_idx, 1);
      if (b_tok != -1 && d_tok != -1 && tokens[b_tok].type == JSMN_STRING &&
          tokens[d_tok].type == JSMN_STRING) {
        get_tok_string_unescape(command_text, &tokens[b_tok], backend,
                                sizeof(backend));
        get_tok_string_unescape(command_text, &tokens[d_tok], device,
                                sizeof(device));
        ok = true;
      }
    }
    if (ok) {
      audio_device_descriptor_t* desc = NULL;
      bool is_capture = (strcmp(simple, "GetCaptureDeviceCapabilities") == 0);
      device_error_t d_err;
      device_error_clear(&d_err);
      bool cap_ok =
          server && server->engine && server->engine->get_device_capabilities &&
          server->engine->get_device_capabilities(server->engine->ctx, backend,
                                                  device, is_capture, &desc, &d_err);
      if (cap_ok && desc) {
        char val[8192];
        format_device_descriptor(desc, val, sizeof(val));
        json_reply(simple, "\"Ok\"", val, out_response, max_len);
        extern void dsp_engine_free_device_capabilities(
            audio_device_descriptor_t * desc);
        dsp_engine_free_device_capabilities(desc);
      } else {
        char err[512];
        const char* err_key = "DeviceError";
        const char* err_msg = d_err.is_error ? d_err.message : "Unknown error";
        if (d_err.is_error) {
          if (d_err.type == DEVICE_ERROR_NOT_FOUND) {
            err_key = "DeviceNotFoundError";
          } else if (d_err.type == DEVICE_ERROR_BUSY) {
            err_key = "DeviceBusyError";
          }
        } else {
          err_key = "DeviceNotFoundError";
          err_msg = device;
        }
        snprintf(err, sizeof(err), "{\"%s\":\"%s\"}", err_key, err_msg);
        json_reply(simple, err, NULL, out_response, max_len);
      }
    } else {
      json_reply(simple,
                 "{\"InvalidRequestError\":\"Could not parse backend and "
                 "device arguments\"}",
                 NULL, out_response, max_len);
    }
  } else if (strcmp(simple, "GetSpectrum") == 0) {
    bool is_capture = true;
    uint32_t channel = 0;
    double min_freq = 20.0;
    double max_freq = 20000.0;
    uint32_t n_bins = 1024;
    bool ok = false;

    if (arg_idx != -1 && tokens[arg_idx].type == JSMN_OBJECT) {
      int ic_idx =
          find_object_key(command_text, tokens, count, arg_idx, "is_capture");
      if (ic_idx != -1)
        is_capture = get_tok_bool(command_text, &tokens[ic_idx]);
      int ch_idx =
          find_object_key(command_text, tokens, count, arg_idx, "channel");
      if (ch_idx != -1)
        channel = (uint32_t)get_tok_int(command_text, &tokens[ch_idx]);
      int mn_idx =
          find_object_key(command_text, tokens, count, arg_idx, "min_freq");
      if (mn_idx != -1)
        min_freq = get_tok_double(command_text, &tokens[mn_idx]);
      int mx_idx =
          find_object_key(command_text, tokens, count, arg_idx, "max_freq");
      if (mx_idx != -1)
        max_freq = get_tok_double(command_text, &tokens[mx_idx]);
      int nb_idx =
          find_object_key(command_text, tokens, count, arg_idx, "n_bins");
      if (nb_idx != -1)
        n_bins = (uint32_t)get_tok_int(command_text, &tokens[nb_idx]);
      ok = true;
    }

    if (ok) {
      spectrum_t spec;
      memset(&spec, 0, sizeof(spec));
      bool spec_ok =
          server && server->engine && server->engine->get_spectrum &&
          server->engine->get_spectrum(server->engine->ctx, is_capture, channel,
                                       min_freq, max_freq, n_bins, &spec);
      if (spec_ok) {
        size_t spec_buf_size = spec.count * 50 + 200;
        char* spec_buf = (char*)malloc(spec_buf_size);
        if (spec_buf) {
          format_spectrum(&spec, spec_buf, spec_buf_size);
          json_reply("GetSpectrum", "\"Ok\"", spec_buf, out_response, max_len);
          free(spec_buf);
        } else {
          json_reply("GetSpectrum", "{\"DeviceError\":\"Out of memory\"}", NULL,
                     out_response, max_len);
        }
        if (spec.frequencies) free(spec.frequencies);
        if (spec.magnitudes) free(spec.magnitudes);
      } else {
        json_reply("GetSpectrum",
                   "{\"DeviceError\":\"Failed to compute spectrum\"}", NULL,
                   out_response, max_len);
      }
    } else {
      json_reply(
          "GetSpectrum",
          "{\"InvalidRequestError\":\"Could not parse GetSpectrum arguments\"}",
          NULL, out_response, max_len);
    }
  } else if (strstr(command_text, "\"ReadConfigJson\"")) {
    char* config_json =
        extract_json_string_value_dyn(command_text, "ReadConfigJson");
    if (config_json) {
      dsp_config_t* parsed = NULL;
      config_error_t cerr;
      memset(&cerr, 0, sizeof(cerr));
      if (config_loader_parse(config_json, &parsed, &cerr) == 0 && parsed) {
        json_reply("ReadConfigJson", "\"Ok\"", config_json, out_response,
                   max_len);
        dsp_config_free(parsed);
      } else {
        char val[600];
        snprintf(val, sizeof(val), "{\"ConfigValidationError\":\"%s\"}",
                 cerr.message);
        json_reply("ReadConfigJson", val, NULL, out_response, max_len);
      }
      free(config_json);
    } else {
      json_reply(
          "ReadConfigJson",
          "{\"InvalidRequestError\":\"Could not parse input config JSON\"}",
          NULL, out_response, max_len);
    }
  } else if (strstr(command_text, "\"ValidateConfigJson\"")) {
    char* config_json =
        extract_json_string_value_dyn(command_text, "ValidateConfigJson");
    if (config_json) {
      dsp_config_t* parsed = NULL;
      config_error_t cerr;
      memset(&cerr, 0, sizeof(cerr));
      if (config_loader_parse(config_json, &parsed, &cerr) == 0 && parsed) {
        json_reply("ValidateConfigJson", "\"Ok\"", NULL, out_response, max_len);
        dsp_config_free(parsed);
      } else {
        char val[600];
        snprintf(val, sizeof(val), "{\"ConfigValidationError\":\"%s\"}",
                 cerr.message);
        json_reply("ValidateConfigJson", val, NULL, out_response, max_len);
      }
      free(config_json);
    } else {
      json_reply(
          "ValidateConfigJson",
          "{\"InvalidRequestError\":\"Could not parse input config JSON\"}",
          NULL, out_response, max_len);
    }
  } else {
    snprintf(out_response, max_len,
             "{\"Invalid\":{\"error\":\"Unsupported command\"}}");
  }
  if (tokens != local_tokens) free(tokens);
}

static void send_websocket_frame(int fd, const char* response) {
  size_t resp_len = strlen(response);
  char frame[16384];
  frame[0] = (char)0x81;
  int header_len = 2;
  if (resp_len < 126) {
    frame[1] = (char)resp_len;
  } else if (resp_len <= 65535) {
    frame[1] = (char)126;
    frame[2] = (char)((resp_len >> 8) & 0xFF);
    frame[3] = (char)(resp_len & 0xFF);
    header_len = 4;
  } else {
    frame[1] = (char)127;
    for (int i = 0; i < 8; i++) {
      frame[2 + i] = (char)((resp_len >> ((7 - i) * 8)) & 0xFF);
    }
    header_len = 10;
  }
  if (header_len + resp_len <= sizeof(frame)) {
    memcpy(&frame[header_len], response, resp_len);
    send(fd, frame, header_len + resp_len, 0);
  } else {
    char* dyn_frame = (char*)malloc(header_len + resp_len);
    if (dyn_frame) {
      dyn_frame[0] = (char)0x81;
      dyn_frame[1] = frame[1];
      memcpy(&dyn_frame[2], &frame[2], header_len - 2);
      memcpy(&dyn_frame[header_len], response, resp_len);
      send(fd, dyn_frame, header_len + resp_len, 0);
      free(dyn_frame);
    }
  }
}

static void* server_thread_func(void* arg) {
  websocket_server_t* server = (websocket_server_t*)arg;
  int client_fds[32];
  char last_state[32][64];
  int num_clients = 0;

  uint64_t last_broadcast_time_ms = 0;

  while (atomic_load_explicit(&server->running, memory_order_acquire)) {
    struct pollfd fds[33];
    fds[0].fd = server->server_fd;
    fds[0].events = POLLIN;
    for (int i = 0; i < num_clients; i++) {
      fds[i + 1].fd = client_fds[i];
      fds[i + 1].events = POLLIN;
    }
    int ret = poll(fds, num_clients + 1, 50);

    // Periodic broadcast tick
    uint64_t now = get_time_ms();
    if (now - last_broadcast_time_ms >= server->update_interval) {
      last_broadcast_time_ms = now;

      state_update_t status;
      memset(&status, 0, sizeof(status));
      bool has_status =
          server->engine && server->engine->get_status &&
          server->engine->get_status(server->engine->ctx, &status);

      const char* state_str = "Inactive";
      if (has_status) {
        state_str = processing_state_to_string(status.state);
      }

      double* current_cap_peak = NULL;
      double* current_cap_rms = NULL;
      double* current_pb_peak = NULL;
      double* current_pb_rms = NULL;
      size_t cap_channels = 0;
      size_t pb_channels = 0;

      processing_parameters_t* params = NULL;
      if (server->engine && server->engine->get_processing_parameters &&
          server->engine->get_processing_parameters(server->engine->ctx,
                                                    (void**)&params) &&
          params) {
        cap_channels = params->capture_channels;
        pb_channels = params->playback_channels;

        if (cap_channels > 0) {
          current_cap_peak = (double*)malloc(cap_channels * sizeof(double));
          current_cap_rms = (double*)malloc(cap_channels * sizeof(double));
          processing_parameters_get_capture_signal_peak(
              params, current_cap_peak, cap_channels);
          processing_parameters_get_capture_signal_rms(params, current_cap_rms,
                                                       cap_channels);

          level_history_append(&server->capture_peak_history, current_cap_peak,
                               cap_channels, now);
          level_history_append(&server->capture_rms_history, current_cap_rms,
                               cap_channels, now);

          if (server->capture_global_peaks_count != cap_channels) {
            server->capture_global_peaks = (double*)realloc(
                server->capture_global_peaks, cap_channels * sizeof(double));
            for (size_t k = server->capture_global_peaks_count;
                 k < cap_channels; k++) {
              if (server->capture_global_peaks)
                server->capture_global_peaks[k] = -1000.0;
            }
            server->capture_global_peaks_count = cap_channels;
          }
          for (size_t k = 0; k < cap_channels; k++) {
            if (server->capture_global_peaks &&
                current_cap_peak[k] > server->capture_global_peaks[k]) {
              server->capture_global_peaks[k] = current_cap_peak[k];
            }
          }
        }

        if (pb_channels > 0) {
          current_pb_peak = (double*)malloc(pb_channels * sizeof(double));
          current_pb_rms = (double*)malloc(pb_channels * sizeof(double));
          processing_parameters_get_playback_signal_peak(
              params, current_pb_peak, pb_channels);
          processing_parameters_get_playback_signal_rms(params, current_pb_rms,
                                                        pb_channels);

          level_history_append(&server->playback_peak_history, current_pb_peak,
                               pb_channels, now);
          level_history_append(&server->playback_rms_history, current_pb_rms,
                               pb_channels, now);

          if (server->playback_global_peaks_count != pb_channels) {
            server->playback_global_peaks = (double*)realloc(
                server->playback_global_peaks, pb_channels * sizeof(double));
            for (size_t k = server->playback_global_peaks_count;
                 k < pb_channels; k++) {
              if (server->playback_global_peaks)
                server->playback_global_peaks[k] = -1000.0;
            }
            server->playback_global_peaks_count = pb_channels;
          }
          for (size_t k = 0; k < pb_channels; k++) {
            if (server->playback_global_peaks &&
                current_pb_peak[k] > server->playback_global_peaks[k]) {
              server->playback_global_peaks[k] = current_pb_peak[k];
            }
          }
        }
      }

      for (int i = 0; i < num_clients; i++) {
        client_session_t* session = &server->client_sessions[i];

        if (session->state_subscribed &&
            strcmp(last_state[i], state_str) != 0) {
          strncpy(last_state[i], state_str, sizeof(last_state[i]) - 1);
          char msg[1024];
          char payload[512];
          format_state_event_payload(status.state, &status.stop_reason, payload,
                                     sizeof(payload));
          snprintf(msg, sizeof(msg),
                   "{\"StateEvent\":{\"result\":\"Ok\",\"value\":%s}}",
                   payload);
          send_websocket_frame(client_fds[i], msg);
        }

        if (session->vu_subscribed && pb_channels > 0) {
          double interval =
              session->vu_max_rate > 0.0 ? 1000.0 / session->vu_max_rate : 0.0;
          if (now - session->last_vu_push_time >= interval) {
            double dt = session->last_vu_push_time == 0
                            ? 100.0
                            : (double)(now - session->last_vu_push_time);
            double attack = smoothing_alpha(dt, session->vu_attack);
            double release = smoothing_alpha(dt, session->vu_release);

            if (session->vu_pb_channels != pb_channels) {
              session->vu_pb_rms = (double*)realloc(
                  session->vu_pb_rms, pb_channels * sizeof(double));
              session->vu_pb_peak = (double*)realloc(
                  session->vu_pb_peak, pb_channels * sizeof(double));
              for (size_t k = 0; k < pb_channels; k++) {
                if (session->vu_pb_rms)
                  session->vu_pb_rms[k] = current_pb_rms[k];
                if (session->vu_pb_peak)
                  session->vu_pb_peak[k] = current_pb_peak[k];
              }
              session->vu_pb_channels = pb_channels;
            } else {
              for (size_t k = 0; k < pb_channels; k++) {
                double prev_amp = db_to_amplitude(session->vu_pb_rms[k]);
                double curr_amp = db_to_amplitude(current_pb_rms[k]);
                double diff = curr_amp - prev_amp;
                if (diff > 0.0)
                  prev_amp += attack * diff;
                else
                  prev_amp += release * diff;
                session->vu_pb_rms[k] = amplitude_to_db(prev_amp);
              }
              for (size_t k = 0; k < pb_channels; k++) {
                double prev_amp = db_to_amplitude(session->vu_pb_peak[k]);
                double curr_amp = db_to_amplitude(current_pb_peak[k]);
                double diff = curr_amp - prev_amp;
                if (diff > 0.0)
                  prev_amp += 1.0 * diff;
                else
                  prev_amp += release * diff;
                session->vu_pb_peak[k] = amplitude_to_db(prev_amp);
              }
            }

            if (cap_channels > 0) {
              if (session->vu_cap_channels != cap_channels) {
                session->vu_cap_rms = (double*)realloc(
                    session->vu_cap_rms, cap_channels * sizeof(double));
                session->vu_cap_peak = (double*)realloc(
                    session->vu_cap_peak, cap_channels * sizeof(double));
                for (size_t k = 0; k < cap_channels; k++) {
                  if (session->vu_cap_rms)
                    session->vu_cap_rms[k] = current_cap_rms[k];
                  if (session->vu_cap_peak)
                    session->vu_cap_peak[k] = current_cap_peak[k];
                }
                session->vu_cap_channels = cap_channels;
              } else {
                for (size_t k = 0; k < cap_channels; k++) {
                  double prev_amp = db_to_amplitude(session->vu_cap_rms[k]);
                  double curr_amp = db_to_amplitude(current_cap_rms[k]);
                  double diff = curr_amp - prev_amp;
                  if (diff > 0.0)
                    prev_amp += attack * diff;
                  else
                    prev_amp += release * diff;
                  session->vu_cap_rms[k] = amplitude_to_db(prev_amp);
                }
                for (size_t k = 0; k < cap_channels; k++) {
                  double prev_amp = db_to_amplitude(session->vu_cap_peak[k]);
                  double curr_amp = db_to_amplitude(current_cap_peak[k]);
                  double diff = curr_amp - prev_amp;
                  if (diff > 0.0)
                    prev_amp += 1.0 * diff;
                  else
                    prev_amp += release * diff;
                  session->vu_cap_peak[k] = amplitude_to_db(prev_amp);
                }
              }
            }

            char* p_rms_str = (char*)malloc(pb_channels * 30 + 10);
            char* p_pk_str = (char*)malloc(pb_channels * 30 + 10);
            char* c_rms_str = (char*)malloc(cap_channels * 30 + 10);
            char* c_pk_str = (char*)malloc(cap_channels * 30 + 10);

            format_double_array(session->vu_pb_rms, pb_channels, p_rms_str,
                                pb_channels * 30 + 10);
            format_double_array(session->vu_pb_peak, pb_channels, p_pk_str,
                                pb_channels * 30 + 10);
            format_double_array(session->vu_cap_rms, cap_channels, c_rms_str,
                                cap_channels * 30 + 10);
            format_double_array(session->vu_cap_peak, cap_channels, c_pk_str,
                                cap_channels * 30 + 10);

            char* msg = (char*)malloc((pb_channels + cap_channels) * 120 + 200);
            sprintf(msg,
                    "{\"VuLevelsEvent\":{\"result\":\"Ok\",\"value\":{"
                    "\"playback_rms\":%s,\"playback_peak\":%s,\"capture_rms\":%"
                    "s,\"capture_peak\":%s}}}",
                    p_rms_str, p_pk_str, c_rms_str, c_pk_str);
            send_websocket_frame(client_fds[i], msg);

            free(msg);
            free(p_rms_str);
            free(p_pk_str);
            free(c_rms_str);
            free(c_pk_str);
            session->last_vu_push_time = now;
          }
        }

        if (session->signal_levels_subscribed) {
          bool send_pb = strcmp(session->signal_levels_side, "playback") == 0 ||
                         strcmp(session->signal_levels_side, "both") == 0;
          bool send_cap = strcmp(session->signal_levels_side, "capture") == 0 ||
                          strcmp(session->signal_levels_side, "both") == 0;

          if (send_pb && pb_channels > 0) {
            char* rms_str = (char*)malloc(pb_channels * 30 + 10);
            char* pk_str = (char*)malloc(pb_channels * 30 + 10);
            format_double_array(current_pb_rms, pb_channels, rms_str,
                                pb_channels * 30 + 10);
            format_double_array(current_pb_peak, pb_channels, pk_str,
                                pb_channels * 30 + 10);

            char* msg = (char*)malloc(pb_channels * 100 + 200);
            sprintf(msg,
                    "{\"SignalLevelsEvent\":{\"result\":\"Ok\",\"value\":{"
                    "\"side\":\"playback\",\"rms\":%s,\"peak\":%s}}}",
                    rms_str, pk_str);
            send_websocket_frame(client_fds[i], msg);

            free(msg);
            free(rms_str);
            free(pk_str);
          }
          if (send_cap && cap_channels > 0) {
            char* rms_str = (char*)malloc(cap_channels * 30 + 10);
            char* pk_str = (char*)malloc(cap_channels * 30 + 10);
            format_double_array(current_cap_rms, cap_channels, rms_str,
                                cap_channels * 30 + 10);
            format_double_array(current_cap_peak, cap_channels, pk_str,
                                cap_channels * 30 + 10);

            char* msg = (char*)malloc(cap_channels * 100 + 200);
            sprintf(msg,
                    "{\"SignalLevelsEvent\":{\"result\":\"Ok\",\"value\":{"
                    "\"side\":\"capture\",\"rms\":%s,\"peak\":%s}}}",
                    rms_str, pk_str);
            send_websocket_frame(client_fds[i], msg);

            free(msg);
            free(rms_str);
            free(pk_str);
          }
        }

        if (session->spectrum_subscribed) {
          double interval =
              session->spectrum_max_rate > 0.0 ? 1000.0 / session->spectrum_max_rate : 0.0;
          if (now - session->last_spectrum_push_time >= interval) {
            spectrum_t spec;
            memset(&spec, 0, sizeof(spec));
            bool spec_ok =
                server && server->engine && server->engine->get_spectrum &&
                server->engine->get_spectrum(server->engine->ctx,
                                             session->spectrum_is_capture,
                                             session->spectrum_channel,
                                             session->spectrum_min_freq,
                                             session->spectrum_max_freq,
                                             session->spectrum_n_bins, &spec);
            if (spec_ok) {
              size_t spec_buf_size = spec.count * 50 + 200;
              char* spec_buf = (char*)malloc(spec_buf_size);
              if (spec_buf) {
                format_spectrum(&spec, spec_buf, spec_buf_size);
                char* msg = (char*)malloc(spec_buf_size + 120);
                sprintf(msg, "{\"SpectrumEvent\":{\"result\":\"Ok\",\"value\":%s}}", spec_buf);
                send_websocket_frame(client_fds[i], msg);
                free(msg);
                free(spec_buf);
              }
              if (spec.frequencies) free(spec.frequencies);
              if (spec.magnitudes) free(spec.magnitudes);
              session->last_spectrum_push_time = now;
            }
          }
        }
      }

      if (current_cap_peak) free(current_cap_peak);
      if (current_cap_rms) free(current_cap_rms);
      if (current_pb_peak) free(current_pb_peak);
      if (current_pb_rms) free(current_pb_rms);
    }

    if (ret > 0) {
      if (fds[0].revents & POLLIN) {
        int cfd = accept(server->server_fd, NULL, NULL);
        if (cfd >= 0 && num_clients < 32) {
          client_fds[num_clients] = cfd;
          last_state[num_clients][0] = '\0';

          client_session_t* session = &server->client_sessions[num_clients];
          memset(session, 0, sizeof(client_session_t));
          uint64_t now_ms = get_time_ms();
          session->last_cap_peak_time = now_ms;
          session->last_cap_rms_time = now_ms;
          session->last_pb_peak_time = now_ms;
          session->last_pb_rms_time = now_ms;

          num_clients++;
        } else if (cfd >= 0) {
          close(cfd);
        }
      }
      for (int i = 0; i < num_clients; i++) {
        if (fds[i + 1].revents & (POLLIN | POLLERR | POLLHUP)) {
          char buf[4096];
          ssize_t n = recv(client_fds[i], buf, sizeof(buf) - 1, 0);
          if (n <= 0) {
            close(client_fds[i]);

            if (server->client_sessions[i].vu_pb_rms)
              free(server->client_sessions[i].vu_pb_rms);
            if (server->client_sessions[i].vu_pb_peak)
              free(server->client_sessions[i].vu_pb_peak);
            if (server->client_sessions[i].vu_cap_rms)
              free(server->client_sessions[i].vu_cap_rms);
            if (server->client_sessions[i].vu_cap_peak)
              free(server->client_sessions[i].vu_cap_peak);

            for (int j = i; j < num_clients - 1; j++) {
              client_fds[j] = client_fds[j + 1];
              strcpy(last_state[j], last_state[j + 1]);
              server->client_sessions[j] = server->client_sessions[j + 1];
            }
            num_clients--;
            i--;
          } else {
            buf[n] = '\0';
            if (strncmp(buf, "GET ", 4) == 0 && strstr(buf, "Upgrade: ")) {
              char* key_ptr = strstr(buf, "Sec-WebSocket-Key: ");
              if (key_ptr) {
                key_ptr += 19;
                char key[64];
                int k = 0;
                while (*key_ptr && *key_ptr != '\r' && *key_ptr != '\n' &&
                       k < 63) {
                  key[k++] = *key_ptr++;
                }
                key[k] = '\0';
                char concat[128];
                snprintf(concat, sizeof(concat),
                         "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
                unsigned char hash[CC_SHA1_DIGEST_LENGTH];
                CC_SHA1(concat, (CC_LONG)strlen(concat), hash);

                static const char b64[] =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz012345"
                    "6789+/";
                char b64_hash[32];
                int b_idx = 0;
                for (int idx = 0; idx < 20; idx += 3) {
                  uint32_t val = (hash[idx] << 16) |
                                 ((idx + 1 < 20 ? hash[idx + 1] : 0) << 8) |
                                 (idx + 2 < 20 ? hash[idx + 2] : 0);
                  b64_hash[b_idx++] = b64[(val >> 18) & 63];
                  b64_hash[b_idx++] = b64[(val >> 12) & 63];
                  b64_hash[b_idx++] =
                      (idx + 1 < 20) ? b64[(val >> 6) & 63] : '=';
                  b64_hash[b_idx++] = (idx + 2 < 20) ? b64[val & 63] : '=';
                }
                b64_hash[b_idx] = '\0';

                char reply[512];
                snprintf(reply, sizeof(reply),
                         "HTTP/1.1 101 Switching Protocols\r\nUpgrade: "
                         "websocket\r\nConnection: "
                         "Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
                         b64_hash);
                send(client_fds[i], reply, strlen(reply), 0);
              }
              continue;
            }

            if ((unsigned char)buf[0] == 0x81 ||
                ((unsigned char)buf[0] & 0x7F) == 0x08) {
              if (((unsigned char)buf[0] & 0x7F) == 0x08) {
                close(client_fds[i]);
                if (server->client_sessions[i].vu_pb_rms)
                  free(server->client_sessions[i].vu_pb_rms);
                if (server->client_sessions[i].vu_pb_peak)
                  free(server->client_sessions[i].vu_pb_peak);
                if (server->client_sessions[i].vu_cap_rms)
                  free(server->client_sessions[i].vu_cap_rms);
                if (server->client_sessions[i].vu_cap_peak)
                  free(server->client_sessions[i].vu_cap_peak);
                for (int j = i; j < num_clients - 1; j++) {
                  client_fds[j] = client_fds[j + 1];
                  strcpy(last_state[j], last_state[j + 1]);
                  server->client_sessions[j] = server->client_sessions[j + 1];
                }
                num_clients--;
                i--;
                continue;
              }
              unsigned char len_byte = (unsigned char)buf[1];
              int payload_len = len_byte & 0x7F;
              int mask_offset = 2;
              if (payload_len == 126) {
                payload_len =
                    ((unsigned char)buf[2] << 8) | (unsigned char)buf[3];
                mask_offset = 4;
              } else if (payload_len == 127) {
                mask_offset = 10;
              }
              unsigned char* mask = (unsigned char*)&buf[mask_offset];
              char* payload = &buf[mask_offset + 4];
              for (int p = 0; p < payload_len && (mask_offset + 4 + p) < n;
                   p++) {
                payload[p] ^= mask[p % 4];
              }
              payload[payload_len] = '\0';

              char response[16384];
              response[0] = '\0';
              websocket_server_handle_command(server, i, payload, response,
                                              sizeof(response));
              if (response[0] != '\0') {
                send_websocket_frame(client_fds[i], response);
              }
              continue;
            }

            char response[16384];
            response[0] = '\0';
            websocket_server_handle_command(server, i, buf, response,
                                            sizeof(response));
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
  if (!server || atomic_load_explicit(&server->running, memory_order_acquire))
    return false;

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
  if (!server || !atomic_load_explicit(&server->running, memory_order_acquire))
    return;
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
  if (server->active_config_json) free(server->active_config_json);
  if (server->active_config_title) free(server->active_config_title);
  if (server->active_config_description)
    free(server->active_config_description);

  // Free level history arrays
  for (size_t i = 0; i < 300; i++) {
    if (server->capture_peak_history.samples[i].levels)
      free(server->capture_peak_history.samples[i].levels);
    if (server->capture_rms_history.samples[i].levels)
      free(server->capture_rms_history.samples[i].levels);
    if (server->playback_peak_history.samples[i].levels)
      free(server->playback_peak_history.samples[i].levels);
    if (server->playback_rms_history.samples[i].levels)
      free(server->playback_rms_history.samples[i].levels);
  }

  // Free global peak arrays
  if (server->capture_global_peaks) free(server->capture_global_peaks);
  if (server->playback_global_peaks) free(server->playback_global_peaks);

  // Free client sessions
  for (size_t i = 0; i < 32; i++) {
    if (server->client_sessions[i].vu_pb_rms)
      free(server->client_sessions[i].vu_pb_rms);
    if (server->client_sessions[i].vu_pb_peak)
      free(server->client_sessions[i].vu_pb_peak);
    if (server->client_sessions[i].vu_cap_rms)
      free(server->client_sessions[i].vu_cap_rms);
    if (server->client_sessions[i].vu_cap_peak)
      free(server->client_sessions[i].vu_cap_peak);
  }

  free(server);
}
#else
// Stub implementations for Windows target
websocket_server_t* websocket_server_create(uint16_t port, const char* host,
                                            active_config_path_t* active_path) {
  (void)port;
  (void)host;
  (void)active_path;
  return NULL;
}
void websocket_server_set_engine(websocket_server_t* server,
                                 dsp_engine_interface_t* engine) {
  (void)server;
  (void)engine;
}
void websocket_server_set_state_file(websocket_server_t* server,
                                     const char* state_file_path) {
  (void)server;
  (void)state_file_path;
}
bool websocket_server_start(websocket_server_t* server) {
  (void)server;
  return false;
}
void websocket_server_stop(websocket_server_t* server) { (void)server; }
void websocket_server_free(websocket_server_t* server) { (void)server; }
void websocket_server_handle_command(websocket_server_t* server, int client_idx,
                                     const char* command_text,
                                     char* out_response, size_t max_len) {
  (void)server;
  (void)client_idx;
  (void)command_text;
  (void)out_response;
  (void)max_len;
}
active_config_path_t* active_config_path_create(const char* initial_path) {
  (void)initial_path;
  return NULL;
}
const char* active_config_path_get(const active_config_path_t* path) {
  (void)path;
  return "";
}
void active_config_path_set(active_config_path_t* path, const char* new_path) {
  (void)path;
  (void)new_path;
}
void active_config_path_free(active_config_path_t* path) { (void)path; }
#endif
