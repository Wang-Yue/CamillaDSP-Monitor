// WebSocket control server
// Provides runtime control API compatible with the control protocol

#include "websocket_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

void dyn_string_init(dyn_string_t* ds, size_t initial_cap) {
  ds->data = (char*)malloc(initial_cap);
  if (ds->data) {
    ds->data[0] = '\0';
    ds->capacity = initial_cap;
  } else {
    ds->capacity = 0;
  }
  ds->length = 0;
}

void dyn_string_free(dyn_string_t* ds) {
  if (ds->data) free(ds->data);
  ds->data = NULL;
  ds->capacity = 0;
  ds->length = 0;
}

static void dyn_string_printf(dyn_string_t* ds, const char* fmt, ...) {
  if (!ds->data || ds->capacity == 0) return;
  va_list args;
  va_start(args, fmt);
  
  va_list args_copy;
  va_copy(args_copy, args);
  int needed = vsnprintf(ds->data, ds->capacity, fmt, args_copy);
  va_end(args_copy);
  
  if (needed < 0) {
    va_end(args);
    return;
  }
  
  if ((size_t)needed >= ds->capacity) {
    size_t new_cap = ds->capacity * 2;
    if (new_cap <= (size_t)needed) new_cap = (size_t)needed + 1;
    char* new_data = (char*)realloc(ds->data, new_cap);
    if (!new_data) {
      va_end(args);
      return;
    }
    ds->data = new_data;
    ds->capacity = new_cap;
    
    vsnprintf(ds->data, ds->capacity, fmt, args);
  }
  ds->length = (size_t)needed;
  va_end(args);
}


#include <pthread.h>
#include <stdatomic.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
typedef SOCKET socket_t;
#else
typedef int socket_t;
#endif

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
  socket_t server_fd;
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

  pthread_mutex_t sessions_mutex;
  /** Array of active client sessions. */
  client_session_t client_sessions[32];
};

#include "Logging/app_logger.h"

static const logger_t server_logger = {"dsp.server.websocket"};

#ifdef _WIN32
#include <ws2tcpip.h>
#define CLOSE_SOCKET(s) closesocket(s)
#define INVALID_SOCKET_VAL INVALID_SOCKET
#define IS_INVALID_SOCKET(s) ((s) == INVALID_SOCKET)
#define IS_SOCKET_ERROR(r) ((r) == SOCKET_ERROR)
#define GET_SOCKET_ERROR() WSAGetLastError()
#define poll_sockets WSAPoll
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCKET(s) close(s)
#define INVALID_SOCKET_VAL (-1)
#define IS_INVALID_SOCKET(s) ((s) < 0)
#define IS_SOCKET_ERROR(r) ((r) < 0)
#define GET_SOCKET_ERROR() errno
#define poll_sockets poll
#endif

#include "Audio/processing_parameters.h"
#include "Config/cJSON.h"
#include "Pipeline/config_loader.h"
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <stdint.h>
#define CC_SHA1_DIGEST_LENGTH 20
typedef uint32_t CC_LONG;

#define SHA1_ROL(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

/**
 * @brief Performs a SHA-1 block transformation.
 *
 * Core SHA-1 compression function. Processes a single 64-byte block to update
 * the 160-bit state. Used for WebSocket handshake key generation on non-macOS
 * systems.
 *
 * @param state SHA-1 state vector (5 words).
 * @param buffer 64-byte block buffer to process.
 */
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

/**
 * @brief Computes SHA-1 hash of the given data buffer.
 *
 * Provides a fallback SHA-1 implementation on systems that do not offer
 * CommonCrypto (like Linux). Outputs a 20-byte digest.
 *
 * @param data Pointer to the input data buffer.
 * @param len Length of input data in bytes.
 * @param digest Output buffer to store the 20-byte SHA-1 digest.
 */
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
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * @brief Converts a decibel (dB) value to a linear amplitude ratio.
 *
 * @param db Value in dB. Values <= -1000.0 are treated as silence (0.0
 * amplitude).
 * @return Linear amplitude ratio.
 */
static double db_to_amplitude(double db) {
  if (db <= -1000.0) return 0.0;
  return pow(10.0, db / 20.0);
}

/**
 * @brief Converts a linear amplitude ratio to a decibel (dB) value.
 *
 * @param amp Linear amplitude ratio. Values <= 0.0 return -1000.0 dB.
 * @return Value in dB. Min value is capped at -1000.0 dB.
 */
static double amplitude_to_db(double amp) {
  if (amp <= 0.0) return -1000.0;
  double db = 20.0 * log10(amp);
  return db < -1000.0 ? -1000.0 : db;
}

/**
 * @brief Appends a new level sample to the history ring buffer.
 *
 * If the channel count changes, the history is cleared and re-initialized.
 *
 * @param history Pointer to the level history structure.
 * @param levels Array of levels per channel.
 * @param channels Number of channels.
 * @param now_ms Current timestamp in milliseconds.
 */
static void level_history_clear(level_history_t* history) {
  if (!history) return;
  for (size_t i = 0; i < 300; i++) {
    if (history->samples[i].levels) {
      free(history->samples[i].levels);
      history->samples[i].levels = NULL;
    }
  }
  history->head = 0;
  history->size = 0;
  history->channels = 0;
}

static void client_session_clear(client_session_t* session) {
  if (!session) return;
  if (session->vu_pb_rms) {
    free(session->vu_pb_rms);
    session->vu_pb_rms = NULL;
  }
  if (session->vu_pb_peak) {
    free(session->vu_pb_peak);
    session->vu_pb_peak = NULL;
  }
  if (session->vu_cap_rms) {
    free(session->vu_cap_rms);
    session->vu_cap_rms = NULL;
  }
  if (session->vu_cap_peak) {
    free(session->vu_cap_peak);
    session->vu_cap_peak = NULL;
  }
  session->vu_pb_channels = 0;
  session->vu_cap_channels = 0;
}

/**
 * @brief Appends a new level sample to the history ring buffer.
 *
 * If the channel count changes, the history is cleared and re-initialized.
 *
 * @param history Pointer to the level history structure.
 * @param levels Array of levels per channel.
 * @param channels Number of channels.
 * @param now_ms Current timestamp in milliseconds.
 */
static void level_history_append(level_history_t* history, const double* levels,
                                 size_t channels, uint64_t now_ms) {
  if (history->channels != channels) {
    level_history_clear(history);
    history->channels = channels;
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

/**
 * @brief Finds the peak (maximum) level for each channel since a given
 * timestamp.
 *
 * Searches the level history ring buffer backward starting from the head.
 *
 * @param history Pointer to the level history structure.
 * @param since_ms Start timestamp in milliseconds.
 * @param out_levels Output array to store the peak levels per channel
 * (initialized to -1000.0 dB).
 */
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

/**
 * @brief Calculates the root-mean-square (RMS) level for each channel since a
 * given timestamp.
 *
 * Aggregates values in the level history ring buffer backward from the head.
 *
 * @param history Pointer to the level history structure.
 * @param since_ms Start timestamp in milliseconds.
 * @param out_levels Output array to store the RMS levels per channel in dB.
 */
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

/**
 * @brief Calculates the smoothing factor (alpha) for an exponential moving
 * average.
 *
 * @param delta_ms Time elapsed since the last update in milliseconds.
 * @param time_constant_ms The response time constant in milliseconds.
 * @return The smoothing factor alpha (between 0.0 and 1.0). Returns 1.0 if time
 * constant is <= 0.
 */
static double smoothing_alpha(double delta_ms, double time_constant_ms) {
  if (time_constant_ms <= 0.0) return 1.0;
  double delta_sec = delta_ms / 1000.0;
  double time_constant_sec = time_constant_ms / 1000.0;
  return 1.0 - exp(-delta_sec / time_constant_sec);
}

websocket_server_t* websocket_server_create(uint16_t port, const char* host) {
  websocket_server_t* server =
      (websocket_server_t*)calloc(1, sizeof(websocket_server_t));
  if (!server) return NULL;
  server->port = port;
  if (host && host[0]) {
    strncpy(server->host, host, sizeof(server->host) - 1);
  } else {
    strncpy(server->host, "127.0.0.1", sizeof(server->host) - 1);
  }
  server->server_fd = INVALID_SOCKET_VAL;
  server->update_interval = 100;
  atomic_init(&server->running, false);

  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&server->sessions_mutex, &attr);
  pthread_mutexattr_destroy(&attr);

  return server;
}

void websocket_server_set_engine(websocket_server_t* server,
                                 dsp_engine_interface_t* engine) {
  if (server) {
    server->engine = engine;
  }
}

/**
 * @brief Gets the current system time in milliseconds.
 *
 * @return Current timestamp in milliseconds.
 */
static uint64_t get_time_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/**
 * @brief Formats a processing stop reason into a quoted JSON string.
 *
 * Writes a description of why processing stopped (e.g. format change, error)
 * into the output buffer.
 *
 * @param reason Pointer to the stop reason structure.
 * @param out Output buffer to write the string into.
 * @param max_len Maximum length of the output buffer.
 */
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

/**
 * @brief Formats the payload for a state event into a JSON object string.
 *
 * Generates an object like: `{"state":"Running","stop_reason":"None"}`.
 *
 * @param state The current processing state.
 * @param reason The stop reason.
 * @param out Output buffer.
 * @param max_len Maximum length of the output buffer.
 */
static void format_state_event_payload(processing_state_t state,
                                       const processing_stop_reason_t* reason,
                                       char* out, size_t max_len) {
  char reason_str[512] = "\"None\"";
  stop_reason_to_string(reason, reason_str, sizeof(reason_str));
  snprintf(out, max_len, "{\"state\":\"%s\",\"stop_reason\":%s}",
           processing_state_to_string(state), reason_str);
}

/**
 * @brief Maps an audio backend error type to a WebSocket control protocol error
 * key.
 *
 * @param type The audio backend error type.
 * @return The corresponding error name string.
 */
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

/**
 * @brief Constructs a standard JSON reply string for the control protocol.
 *
 * Formats a reply of the form:
 * `{"Command":{"result":RESULT_STR,"value":VALUE_STR}}`.
 *
 * @param cmd The command name.
 * @param res_str The result status string (e.g. `"Ok"` or error string).
 * @param val_str Optional payload value string (can be NULL or empty).
 * @param out Output buffer.
 * @param max_len Maximum length of the output buffer.
 */
static void json_reply(const char* cmd, const char* res_str,
                       const char* val_str, dyn_string_t* ds) {
  if (val_str && val_str[0]) {
    dyn_string_printf(ds, "{\"%s\":{\"result\":%s,\"value\":%s}}", cmd,
                      res_str, val_str);
  } else {
    dyn_string_printf(ds, "{\"%s\":{\"result\":%s}}", cmd, res_str);
  }
}

/**
 * @brief Reads a file into a dynamically allocated string (server helper).
 *
 * @param path Path to the file.
 * @return Pointer to the allocated string containing the file contents, or NULL
 * if reading fails.
 */
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

/**
 * @brief Extracts a string value from a flat JSON object by its key.
 *
 * Helper to quickly read simple string fields from JSON without manual cJSON
 * traversal.
 *
 * @param json The JSON string.
 * @param key The key to look up.
 * @param out_buf Output buffer to copy the string into.
 * @param max_len Size of the output buffer.
 * @return true if the key was found and value copied, false otherwise.
 */
static bool extract_json_string_value(const char* json, const char* key,
                                      char* out_buf, size_t max_len) {
  if (!json || !key || !out_buf || max_len == 0) return false;
  cJSON* root = cJSON_Parse(json);
  if (!root) return false;
  cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
  bool success = false;
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(out_buf, item->valuestring, max_len - 1);
    out_buf[max_len - 1] = '\0';
    success = true;
  }
  cJSON_Delete(root);
  return success;
}

/**
 * @brief Locates a cJSON node matching a RFC 6901 JSON pointer.
 *
 * Recursively navigates through JSON objects and arrays matching segments of
 * the pointer.
 *
 * @param root The root cJSON node.
 * @param pointer The JSON pointer string (e.g. "/devices/playback/device").
 * @param out_parent Optional output pointer to store the located node's parent.
 * @param out_key Optional output pointer to store the key matching the node in
 * its parent.
 * @param out_index Optional output pointer to store the index matching the node
 * in its parent array.
 * @return The located cJSON node, or NULL if not found.
 */
static cJSON* cjson_locate_pointer(cJSON* root, const char* pointer,
                                   cJSON** out_parent, const char** out_key,
                                   int* out_index) {
  if (!root || !pointer) return NULL;
  const char* ptr = pointer;
  if (*ptr == '/') ptr++;
  cJSON* curr = root;
  cJSON* parent = NULL;
  const char* last_key = NULL;
  int last_idx = -1;

  while (*ptr && curr) {
    char segment[128];
    size_t seg_len = 0;
    while (*ptr && *ptr != '/' && seg_len < sizeof(segment) - 1) {
      segment[seg_len++] = *ptr++;
    }
    segment[seg_len] = '\0';
    if (*ptr == '/') ptr++;

    parent = curr;
    if (cJSON_IsObject(curr)) {
      cJSON* child = curr->child;
      curr = NULL;
      last_key = NULL;
      while (child) {
        if (strcmp(child->string, segment) == 0) {
          curr = child;
          last_key = child->string;
          break;
        }
        child = child->next;
      }
      last_idx = -1;
    } else if (cJSON_IsArray(curr)) {
      char* endptr = NULL;
      int idx = (int)strtol(segment, &endptr, 10);
      if (endptr == segment || *endptr != '\0') return NULL;
      curr = cJSON_GetArrayItem(curr, idx);
      last_idx = idx;
      last_key = NULL;
    } else {
      return NULL;
    }
  }

  if (out_parent) *out_parent = parent;
  if (out_key) *out_key = last_key;
  if (out_index) *out_index = last_idx;
  return curr;
}

/**
 * @brief Extracts the JSON fragment at the specified JSON pointer as a string.
 *
 * @param json The source JSON string.
 * @param pointer JSON pointer.
 * @param out_val Output buffer to copy the printed JSON fragment.
 * @param max_len Size of the output buffer.
 * @return true if successful, false otherwise.
 */
static bool server_get_value_at_pointer(const char* json, const char* pointer,
                                        char* out_val, size_t max_len) {
  cJSON* root = cJSON_Parse(json);
  if (!root) return false;
  cJSON* node = cjson_locate_pointer(root, pointer, NULL, NULL, NULL);
  if (!node) {
    cJSON_Delete(root);
    return false;
  }
  char* printed = cJSON_PrintUnformatted(node);
  if (printed) {
    strncpy(out_val, printed, max_len - 1);
    out_val[max_len - 1] = '\0';
    free(printed);
    cJSON_Delete(root);
    return true;
  }
  cJSON_Delete(root);
  return false;
}

/**
 * @brief Replaces or inserts a JSON value at the specified JSON pointer
 * location.
 *
 * Parses the new value string, locates the parent at the pointer, and replaces
 * the element. Returns a newly allocated string containing the updated JSON.
 *
 * @param json The source JSON string.
 * @param pointer JSON pointer.
 * @param new_val_str The new value as a JSON fragment string.
 * @return A newly allocated string containing the serialized updated JSON, or
 * NULL on failure.
 */
static char* server_set_value_at_pointer_str(const char* json,
                                             const char* pointer,
                                             const char* new_val_str) {
  cJSON* root = cJSON_Parse(json);
  if (!root) return NULL;

  cJSON* parent = NULL;
  const char* key = NULL;
  int idx = -1;
  cJSON* target = cjson_locate_pointer(root, pointer, &parent, &key, &idx);
  (void)target;
  if (!parent) {
    cJSON_Delete(root);
    return NULL;
  }

  cJSON* new_node = cJSON_Parse(new_val_str);
  if (!new_node) {
    cJSON_Delete(root);
    return NULL;
  }

  if (key) {
    cJSON_ReplaceItemInObject(parent, key, new_node);
  } else if (idx != -1) {
    cJSON_ReplaceItemInArray(parent, idx, new_node);
  } else {
    cJSON_Delete(new_node);
    cJSON_Delete(root);
    return NULL;
  }

  char* updated_json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return updated_json;
}

/**
 * @brief Applies a JSON Merge Patch (RFC 7396) to a target cJSON object.
 *
 * Modifies the target cJSON object in-place according to the rules of JSON
 * Merge Patch. Null values in the patch delete corresponding fields in the
 * target.
 *
 * @param target The target cJSON object to patch.
 * @param patch The patch cJSON object.
 */
static void cjson_merge_patch(cJSON* target, const cJSON* patch) {
  if (!target || !patch) return;
  if (!cJSON_IsObject(target) || !cJSON_IsObject(patch)) return;

  cJSON* patch_child = patch->child;
  while (patch_child) {
    cJSON* target_child =
        cJSON_GetObjectItemCaseSensitive(target, patch_child->string);
    if (cJSON_IsNull(patch_child)) {
      if (target_child) {
        cJSON_DeleteItemFromObject(target, patch_child->string);
      }
    } else if (cJSON_IsObject(patch_child)) {
      if (target_child && cJSON_IsObject(target_child)) {
        cjson_merge_patch(target_child, patch_child);
      } else {
        cJSON* duplicated = cJSON_Duplicate(patch_child, true);
        if (target_child) {
          cJSON_ReplaceItemInObject(target, patch_child->string, duplicated);
        } else {
          cJSON_AddItemToObject(target, patch_child->string, duplicated);
        }
      }
    } else {
      cJSON* duplicated = cJSON_Duplicate(patch_child, true);
      if (target_child) {
        cJSON_ReplaceItemInObject(target, patch_child->string, duplicated);
      } else {
        cJSON_AddItemToObject(target, patch_child->string, duplicated);
      }
    }
    patch_child = patch_child->next;
  }
}

/**
 * @brief Formats an array of doubles as a JSON array string.
 *
 * Outputs a string like: `[-3.14, 0.0, 1.23]`.
 *
 * @param arr Array of doubles.
 * @param count Number of elements in the array.
 * @param out Output buffer.
 * @param max_len Maximum length of the output buffer.
 */
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

/**
 * @brief Serializes an audio device descriptor struct into its JSON
 * representation.
 *
 * Formats details of the device's name, capabilities, sample rates, formats and
 * channels.
 *
 * @param desc Pointer to the device descriptor.
 * @param out Output buffer.
 * @param max_len Maximum length of the output buffer.
 */
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

/**
 * @brief Serializes a frequency spectrum structure into a JSON object string.
 *
 * Formats frequencies and magnitudes arrays.
 *
 * @param spec Pointer to the spectrum structure.
 * @param out Output buffer.
 * @param max_len Maximum length of the output buffer.
 */
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

/**
 * @brief Helper to handle volume adjustments (relative change) on a specific
 * fader.
 *
 * Validates the current running status, fetches processing parameters, computes
 * the new volume clamped to the bounds, applies it, and formats the JSON
 * response.
 *
 * @param server Pointer to the WebSocket server.
 * @param fader The fader index to adjust.
 * @param delta The volume change in dB.
 * @param min_vol Minimum volume limit in dB.
 * @param max_vol Maximum volume limit in dB.
 * @param out_response Output buffer for the JSON reply.
 * @param max_len Maximum length of the response buffer.
 * @param cmd_name Command name used for the reply key.
 * @return true if handled, false otherwise.
 */
static bool server_handle_adjust_volume_fader(
    websocket_server_t* server, fader_t fader, double delta, double min_vol,
    double max_vol, dyn_string_t* ds, const char* cmd_name) {
  processing_parameters_t* params = NULL;
  if (!server || !server->engine ||
      !server->engine->get_processing_parameters ||
      !server->engine->get_processing_parameters(server->engine->ctx,
                                                 (void**)&params) ||
      !params) {
    json_reply(cmd_name, "\"ProcessingNotRunningError\"", NULL, ds);
    return true;
  }

  double current =
      processing_parameters_get_target_volume_for_fader(params, fader);
  double new_vol = current + delta;
  if (new_vol < min_vol) new_vol = min_vol;
  if (new_vol > max_vol) new_vol = max_vol;

  if (server && server->engine && server->engine->set_fader_volume) {
    server->engine->set_fader_volume(server->engine->ctx, fader, (float)new_vol,
                                     false);
  }

  char val[64];
  if (fader == FADER_MAIN) {
    snprintf(val, sizeof(val), "%.17g", new_vol);
  } else {
    snprintf(val, sizeof(val), "[%d,%.17g]", (int)fader, new_vol);
  }
  json_reply(cmd_name, "\"Ok\"", val, ds);
  return true;
}

// MARK: - Command Handler

/// Handle a control command text (either simple quoted string or JSON object)
/// and populate out_response.
void websocket_server_handle_command(websocket_server_t* server, int client_idx,
                                     const char* command_text,
                                     dyn_string_t* ds) {
  if (!server || !ds || !command_text) return;

  cJSON* root = cJSON_Parse(command_text);
  if (!root) {
    json_reply("Invalid", "{\"error\":\"Invalid JSON\"}", NULL, ds);
    return;
  }

  pthread_mutex_lock(&server->sessions_mutex);

  char cmd_name[128] = "";
  cJSON* arg = NULL;

  if (cJSON_IsString(root)) {
    strncpy(cmd_name, root->valuestring, sizeof(cmd_name) - 1);
  } else if (cJSON_IsObject(root)) {
    cJSON* child = root->child;
    if (child) {
      strncpy(cmd_name, child->string, sizeof(cmd_name) - 1);
      arg = child;
    }
  }

  const char* simple = cmd_name;

  if (strcmp(simple, "GetVersion") == 0) {
    json_reply("GetVersion", "\"Ok\"", "\"CamillaDSP-C-Embedded 2.0.0\"", ds);
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
    json_reply("GetState", "\"Ok\"", val, ds);
  } else if (strcmp(simple, "GetStopReason") == 0) {
    char reason_str[512] = "\"None\"";
    if (server && server->engine && server->engine->get_status) {
      state_update_t status;
      if (server->engine->get_status(server->engine->ctx, &status)) {
        stop_reason_to_string(&status.stop_reason, reason_str,
                              sizeof(reason_str));
      }
    }
    json_reply("GetStopReason", "\"Ok\"", reason_str, ds);
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
      json_reply("GetVolume", "\"Ok\"", val, ds);
    } else {
      json_reply("GetVolume", "\"ProcessingNotRunningError\"", NULL, ds);
    }
  } else if (strcmp(simple, "GetMute") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      bool muted = processing_parameters_is_muted_for_fader(params, FADER_MAIN);
      json_reply("GetMute", "\"Ok\"", muted ? "true" : "false", ds);
    } else {
      json_reply("GetMute", "\"ProcessingNotRunningError\"", NULL, ds);
    }
  } else if (strcmp(simple, "ToggleMute") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      bool was_muted =
          processing_parameters_is_muted_for_fader(params, FADER_MAIN);
      if (server && server->engine && server->engine->set_fader_mute) {
        server->engine->set_fader_mute(server->engine->ctx, FADER_MAIN,
                                       !was_muted);
      }
      json_reply("ToggleMute", "\"Ok\"", !was_muted ? "true" : "false", ds);
    } else {
      json_reply("ToggleMute", "\"ProcessingNotRunningError\"", NULL, ds);
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
      json_reply("GetFaders", "\"Ok\"", faders_val, ds);
    } else {
      json_reply("GetFaders", "\"ProcessingNotRunningError\"", NULL, ds);
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
        json_reply("GetCaptureSignalRms", "\"Ok\"", val, ds);
      } else {
        json_reply("GetCaptureSignalRms", "\"Ok\"", "[]", ds);
      }
    } else {
      json_reply("GetCaptureSignalRms", "\"ProcessingNotRunningError\"", NULL, ds);
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
        json_reply("GetCaptureSignalPeak", "\"Ok\"", val, ds);
      } else {
        json_reply("GetCaptureSignalPeak", "\"Ok\"", "[]", ds);
      }
    } else {
      json_reply("GetCaptureSignalPeak", "\"ProcessingNotRunningError\"", NULL, ds);
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
        json_reply("GetPlaybackSignalRms", "\"Ok\"", val, ds);
      } else {
        json_reply("GetPlaybackSignalRms", "\"Ok\"", "[]", ds);
      }
    } else {
      json_reply("GetPlaybackSignalRms", "\"ProcessingNotRunningError\"", NULL, ds);
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
        json_reply("GetPlaybackSignalPeak", "\"Ok\"", val, ds);
      } else {
        json_reply("GetPlaybackSignalPeak", "\"Ok\"", "[]", ds);
      }
    } else {
      json_reply("GetPlaybackSignalPeak", "\"ProcessingNotRunningError\"", NULL, ds);
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
      json_reply("GetCaptureRate", "\"Ok\"", val, ds);
    } else {
      json_reply("GetCaptureRate", "\"Ok\"", "0", ds);
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
      json_reply("GetRateAdjust", "\"Ok\"", val, ds);
    } else {
      json_reply("GetRateAdjust", "\"Ok\"", "1.0", ds);
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
      json_reply("GetBufferLevel", "\"Ok\"", val, ds);
    } else {
      json_reply("GetBufferLevel", "\"Ok\"", "0", ds);
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
      json_reply("GetClippedSamples", "\"Ok\"", val, ds);
    } else {
      json_reply("GetClippedSamples", "\"Ok\"", "0", ds);
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
    json_reply("ResetClippedSamples", "\"Ok\"", NULL, ds);
  } else if (strcmp(simple, "GetProcessingLoad") == 0) {
    processing_parameters_t* params = NULL;
    if (server && server->engine && server->engine->get_processing_parameters &&
        server->engine->get_processing_parameters(server->engine->ctx,
                                                  (void**)&params) &&
        params) {
      double load = atomic_double_get(&params->processing_load);
      char val[32];
      snprintf(val, sizeof(val), "%.17g", load);
      json_reply("GetProcessingLoad", "\"Ok\"", val, ds);
    } else {
      json_reply("GetProcessingLoad", "\"Ok\"", "0.0", ds);
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
      json_reply("GetResamplerLoad", "\"Ok\"", val, ds);
    } else {
      json_reply("GetResamplerLoad", "\"Ok\"", "0.0", ds);
    }
  } else if (strcmp(simple, "GetSupportedDeviceTypes") == 0) {
    json_reply("GetSupportedDeviceTypes", "\"Ok\"",
               "[[\"CoreAudio\"],[\"CoreAudio\"]]", ds);
  } else if (strcmp(simple, "GetUpdateInterval") == 0) {
    char val[32];
    snprintf(val, sizeof(val), "%d", server ? server->update_interval : 100);
    json_reply("GetUpdateInterval", "\"Ok\"", val, ds);
  } else if (strcmp(simple, "SetUpdateInterval") == 0) {
    if (arg && cJSON_IsNumber(arg)) {
      double val = arg->valuedouble;
      if (val >= 0.0) {
        if (server) server->update_interval = (uint32_t)val;
        json_reply("SetUpdateInterval", "\"Ok\"", NULL, ds);
      } else {
        json_reply("SetUpdateInterval",
                   "{\"InvalidValueError\":\"Value must be >= 0\"}", NULL, ds);
      }
    } else {
      json_reply("SetUpdateInterval",
                 "{\"InvalidRequestError\":\"Could not parse SetUpdateInterval "
                 "argument\"}",
                 NULL, ds);
    }
  } else if (strcmp(simple, "SubscribeState") == 0) {
    if (server) {
      server->client_sessions[client_idx].state_subscribed = true;
    }
    json_reply("SubscribeState", "\"Ok\"", NULL, ds);
  } else if (strcmp(simple, "SubscribeVuLevels") == 0) {
    double max_rate = 0.0;
    double attack = 0.0;
    double release = 0.0;
    if (arg && cJSON_IsObject(arg)) {
      cJSON* item;
      item = cJSON_GetObjectItemCaseSensitive(arg, "max_rate");
      if (item && cJSON_IsNumber(item)) max_rate = item->valuedouble;
      item = cJSON_GetObjectItemCaseSensitive(arg, "attack");
      if (item && cJSON_IsNumber(item)) attack = item->valuedouble;
      item = cJSON_GetObjectItemCaseSensitive(arg, "release");
      if (item && cJSON_IsNumber(item)) release = item->valuedouble;
    }
    if (attack < 0.0 || attack > 60000.0 || release < 0.0 ||
        release > 60000.0) {
      json_reply("SubscribeVuLevels",
                 "{\"InvalidValueError\":\"attack and release must be between "
                 "0 and 60000 ms\"}",
                 NULL, ds);
    } else {
      if (server) {
        server->client_sessions[client_idx].vu_subscribed = true;
        server->client_sessions[client_idx].vu_max_rate = max_rate;
        server->client_sessions[client_idx].vu_attack = attack;
        server->client_sessions[client_idx].vu_release = release;
        server->client_sessions[client_idx].last_vu_push_time = 0;
      }
      json_reply("SubscribeVuLevels", "\"Ok\"", NULL, ds);
    }
  } else if (strcmp(simple, "SubscribeSignalLevels") == 0) {
    char side[16] = "";
    if (arg && cJSON_IsString(arg) && arg->valuestring) {
      strncpy(side, arg->valuestring, sizeof(side) - 1);
    }
    if (strcmp(side, "playback") == 0 || strcmp(side, "capture") == 0 ||
        strcmp(side, "both") == 0) {
      if (server) {
        server->client_sessions[client_idx].signal_levels_subscribed = true;
        snprintf(server->client_sessions[client_idx].signal_levels_side,
                 sizeof(server->client_sessions[client_idx].signal_levels_side),
                 "%s", side);
      }
      json_reply("SubscribeSignalLevels", "\"Ok\"", NULL, ds);
    } else {
      json_reply("SubscribeSignalLevels",
                 "{\"InvalidValueError\":\"side must be playback, capture, "
                 "or both\"}",
                 NULL, ds);
    }
  } else if (strcmp(simple, "SubscribeSpectrum") == 0) {
    bool is_capture = true;
    uint32_t channel = 0;
    double min_freq = 20.0;
    double max_freq = 20000.0;
    uint32_t n_bins = 1024;
    double max_rate = 0.0;
    bool ok = false;

    if (arg && cJSON_IsObject(arg)) {
      cJSON* item;
      item = cJSON_GetObjectItemCaseSensitive(arg, "is_capture");
      if (item && cJSON_IsBool(item)) is_capture = cJSON_IsTrue(item);
      item = cJSON_GetObjectItemCaseSensitive(arg, "channel");
      if (item && cJSON_IsNumber(item)) channel = (uint32_t)item->valueint;
      item = cJSON_GetObjectItemCaseSensitive(arg, "min_freq");
      if (item && cJSON_IsNumber(item)) min_freq = item->valuedouble;
      item = cJSON_GetObjectItemCaseSensitive(arg, "max_freq");
      if (item && cJSON_IsNumber(item)) max_freq = item->valuedouble;
      item = cJSON_GetObjectItemCaseSensitive(arg, "n_bins");
      if (item && cJSON_IsNumber(item)) n_bins = (uint32_t)item->valueint;
      item = cJSON_GetObjectItemCaseSensitive(arg, "max_rate");
      if (item && cJSON_IsNumber(item)) max_rate = item->valuedouble;
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
      json_reply("SubscribeSpectrum", "\"Ok\"", NULL, ds);
    } else {
      json_reply("SubscribeSpectrum",
                 "{\"InvalidRequestError\":\"Could not parse SubscribeSpectrum "
                 "arguments\"}",
                 NULL, ds);
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
        json_reply("StopSubscription", "\"Ok\"", NULL, ds);
      } else {
        json_reply("StopSubscription",
                   "{\"InvalidRequestError\":\"No active subscription\"}", NULL, ds);
      }
    } else {
      json_reply("StopSubscription",
                 "{\"InvalidRequestError\":\"No active subscription\"}", NULL, ds);
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
      json_reply("GetCaptureSignalRmsSinceLast", "\"Ok\"", rms_str, ds);
      free(rms_str);
      free(rms);
    } else {
      json_reply("GetCaptureSignalRmsSinceLast",
                 "\"ProcessingNotRunningError\"", NULL, ds);
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
      json_reply("GetCaptureSignalPeakSinceLast", "\"Ok\"", pk_str, ds);
      free(pk_str);
      free(pk);
    } else {
      json_reply("GetCaptureSignalPeakSinceLast",
                 "\"ProcessingNotRunningError\"", NULL, ds);
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
      json_reply("GetPlaybackSignalRmsSinceLast", "\"Ok\"", rms_str, ds);
      free(rms_str);
      free(rms);
    } else {
      json_reply("GetPlaybackSignalRmsSinceLast",
                 "\"ProcessingNotRunningError\"", NULL, ds);
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
      json_reply("GetPlaybackSignalPeakSinceLast", "\"Ok\"", pk_str, ds);
      free(pk_str);
      free(pk);
    } else {
      json_reply("GetPlaybackSignalPeakSinceLast",
                 "\"ProcessingNotRunningError\"", NULL, ds);
    }
  } else if (strcmp(simple, "GetCaptureSignalRmsSince") == 0) {
    double secs = 0;
    if (arg && cJSON_IsNumber(arg)) {
      secs = arg->valuedouble;
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
        json_reply("GetCaptureSignalRmsSince", "\"Ok\"", rms_str, ds);
        free(rms_str);
        free(rms);
      } else {
        json_reply("GetCaptureSignalRmsSince", "\"ProcessingNotRunningError\"",
                   NULL, ds);
      }
    } else {
      json_reply("GetCaptureSignalRmsSince",
                 "{\"InvalidRequestError\":\"Could not parse seconds\"}", NULL, ds);
    }
  } else if (strcmp(simple, "GetCaptureSignalPeakSince") == 0) {
    double secs = 0;
    if (arg && cJSON_IsNumber(arg)) {
      secs = arg->valuedouble;
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
        json_reply("GetCaptureSignalPeakSince", "\"Ok\"", pk_str, ds);
        free(pk_str);
        free(pk);
      } else {
        json_reply("GetCaptureSignalPeakSince", "\"ProcessingNotRunningError\"",
                   NULL, ds);
      }
    } else {
      json_reply("GetCaptureSignalPeakSince",
                 "{\"InvalidRequestError\":\"Could not parse seconds\"}", NULL, ds);
    }
  } else if (strcmp(simple, "GetPlaybackSignalRmsSince") == 0) {
    double secs = 0;
    if (arg && cJSON_IsNumber(arg)) {
      secs = arg->valuedouble;
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
        json_reply("GetPlaybackSignalRmsSince", "\"Ok\"", rms_str, ds);
        free(rms_str);
        free(rms);
      } else {
        json_reply("GetPlaybackSignalRmsSince", "\"ProcessingNotRunningError\"",
                   NULL, ds);
      }
    } else {
      json_reply("GetPlaybackSignalRmsSince",
                 "{\"InvalidRequestError\":\"Could not parse seconds\"}", NULL, ds);
    }
  } else if (strcmp(simple, "GetPlaybackSignalPeakSince") == 0) {
    double secs = 0;
    if (arg && cJSON_IsNumber(arg)) {
      secs = arg->valuedouble;
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
        json_reply("GetPlaybackSignalPeakSince", "\"Ok\"", pk_str, ds);
        free(pk_str);
        free(pk);
      } else {
        json_reply("GetPlaybackSignalPeakSince",
                   "\"ProcessingNotRunningError\"", NULL, ds);
      }
    } else {
      json_reply("GetPlaybackSignalPeakSince",
                 "{\"InvalidRequestError\":\"Could not parse seconds\"}", NULL, ds);
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
            json_reply("GetSignalLevels", "\"Ok\"", val, ds);
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
      json_reply("GetSignalLevels", "\"ProcessingNotRunningError\"", NULL, ds);
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
      json_reply("GetSignalLevelsSinceLast", "\"Ok\"", val, ds);
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
                 NULL, ds);
    }
  } else if (strcmp(simple, "GetSignalLevelsSince") == 0) {
    double secs = 0;
    if (arg && cJSON_IsNumber(arg)) {
      secs = arg->valuedouble;
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
        json_reply("GetSignalLevelsSince", "\"Ok\"", val, ds);
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
                   NULL, ds);
      }
    } else {
      json_reply("GetSignalLevelsSince",
                 "{\"InvalidRequestError\":\"Could not parse seconds\"}", NULL, ds);
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
    json_reply("GetSignalPeaksSinceStart", "\"Ok\"", val, ds);
  } else if (strcmp(simple, "ResetSignalPeaksSinceStart") == 0) {
    for (size_t i = 0; i < server->capture_global_peaks_count; i++) {
      server->capture_global_peaks[i] = -1000.0;
    }
    for (size_t i = 0; i < server->playback_global_peaks_count; i++) {
      server->playback_global_peaks[i] = -1000.0;
    }
    json_reply("ResetSignalPeaksSinceStart", "\"Ok\"", NULL, ds);
  } else if (strcmp(simple, "GetChannelLabels") == 0) {
    char* json = NULL;
    if (server && server->engine && server->engine->get_active_config_json) {
      server->engine->get_active_config_json(server->engine->ctx, &json);
    }
    if (!json) {
      char* path = NULL;
      if (server && server->engine && server->engine->get_config_path) {
        path = server->engine->get_config_path(server->engine->ctx);
      }
      if (path) {
        json = server_read_file_to_string(path);
        free(path);
      }
    }
    char play_labels[2048] = "null";
    char cap_labels[2048] = "null";
    if (json) {
      server_get_value_at_pointer(json, "/devices/playback/labels", play_labels,
                                  sizeof(play_labels));
      server_get_value_at_pointer(json, "/devices/capture/labels", cap_labels,
                                  sizeof(cap_labels));
    }
    char val[4096];
    snprintf(val, sizeof(val), "{\"playback\":%s,\"capture\":%s}", play_labels,
             cap_labels);
    json_reply("GetChannelLabels", "\"Ok\"", val, ds);
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
      json_reply("GetSignalRange", "\"Ok\"", val, ds);
    } else {
      json_reply("GetSignalRange", "\"ProcessingNotRunningError\"", NULL, ds);
    }
  } else if (strcmp(simple, "GetConfigFilePath") == 0) {
    char* path = NULL;
    if (server && server->engine && server->engine->get_config_path) {
      path = server->engine->get_config_path(server->engine->ctx);
    }
    char val[1100];
    if (path) {
      snprintf(val, sizeof(val), "\"%s\"", path);
      free(path);
    } else {
      snprintf(val, sizeof(val), "null");
    }
    json_reply("GetConfigFilePath", "\"Ok\"", val, ds);
  } else if (strcmp(simple, "GetPreviousConfig") == 0) {
    char* prev = NULL;
    if (server && server->engine && server->engine->get_previous_config_json) {
      server->engine->get_previous_config_json(server->engine->ctx, &prev);
    }
    if (prev) {
      json_reply("GetPreviousConfig", "\"Ok\"", prev, ds);
      free(prev);
    } else {
      json_reply("GetPreviousConfig", "\"Ok\"", "null", ds);
    }
  } else if (strcmp(simple, "GetStateFilePath") == 0) {
    const char* path = NULL;
    if (server && server->engine && server->engine->get_state_file) {
      path = server->engine->get_state_file(server->engine->ctx);
    }
    char val[1100];
    if (path)
      snprintf(val, sizeof(val), "\"%s\"", path);
    else
      snprintf(val, sizeof(val), "null");
    json_reply("GetStateFilePath", "\"Ok\"", val, ds);
  } else if (strcmp(simple, "GetStateFileUpdated") == 0) {
    bool updated = server && server->engine && server->engine->is_state_dirty
                       ? !server->engine->is_state_dirty(server->engine->ctx)
                       : true;
    json_reply("GetStateFileUpdated", "\"Ok\"", updated ? "true" : "false", ds);
  } else if (strcmp(simple, "GetConfig") == 0 ||
             strcmp(simple, "GetConfigJson") == 0) {
    char* json = NULL;
    if (server && server->engine && server->engine->get_active_config_json) {
      server->engine->get_active_config_json(server->engine->ctx, &json);
    }
    if (!json) {
      char* path = NULL;
      if (server && server->engine && server->engine->get_config_path) {
        path = server->engine->get_config_path(server->engine->ctx);
      }
      if (path) {
        json = server_read_file_to_string(path);
        free(path);
      }
    }
    if (json) {
      json_reply(simple, "\"Ok\"", json, ds);
      free(json);
    } else {
      json_reply(simple, "{\"InvalidRequestError\":\"No active config\"}", NULL, ds);
    }
  } else if (strcmp(simple, "GetConfigTitle") == 0) {
    char* json = NULL;
    if (server && server->engine && server->engine->get_active_config_json) {
      server->engine->get_active_config_json(server->engine->ctx, &json);
    }
    if (!json) {
      char* path = NULL;
      if (server && server->engine && server->engine->get_config_path) {
        path = server->engine->get_config_path(server->engine->ctx);
      }
      if (path) {
        json = server_read_file_to_string(path);
        free(path);
      }
    }
    char title[256];
    if (json &&
        extract_json_string_value(json, "title", title, sizeof(title))) {
      char val[300];
      snprintf(val, sizeof(val), "\"%s\"", title);
      json_reply("GetConfigTitle", "\"Ok\"", val, ds);
    } else {
      json_reply("GetConfigTitle", "\"Ok\"", "null", ds);
    }
    if (json) free(json);
  } else if (strcmp(simple, "GetConfigDescription") == 0) {
    char* json = NULL;
    if (server && server->engine && server->engine->get_active_config_json) {
      server->engine->get_active_config_json(server->engine->ctx, &json);
    }
    if (!json) {
      char* path = NULL;
      if (server && server->engine && server->engine->get_config_path) {
        path = server->engine->get_config_path(server->engine->ctx);
      }
      if (path) {
        json = server_read_file_to_string(path);
        free(path);
      }
    }
    char desc[512];
    if (json &&
        extract_json_string_value(json, "description", desc, sizeof(desc))) {
      char val[600];
      snprintf(val, sizeof(val), "\"%s\"", desc);
      json_reply("GetConfigDescription", "\"Ok\"", val, ds);
    } else {
      json_reply("GetConfigDescription", "\"Ok\"", "null", ds);
    }
    if (json) free(json);
  } else if (strcmp(simple, "Reload") == 0) {
    char* path = NULL;
    if (server && server->engine && server->engine->get_config_path) {
      path = server->engine->get_config_path(server->engine->ctx);
    }
    if (path) {
      char* json = server_read_file_to_string(path);
      free(path);
      if (json) {
        audio_backend_error_t err;
        memset(&err, 0, sizeof(err));
        bool ok =
            server && server->engine && server->engine->set_config_json &&
            server->engine->set_config_json(server->engine->ctx, json, &err);
        if (ok) {
          json_reply("Reload", "\"Ok\"", NULL, ds);
        } else {
          char val[600];
          snprintf(val, sizeof(val), "{\"%s\":\"%s\"}",
                   get_websocket_error_key(err.type), err.message);
          json_reply("Reload", val, NULL, ds);
        }
        free(json);
      } else {
        json_reply("Reload",
                   "{\"ConfigReadError\":\"Could not read config file\"}", NULL, ds);
      }
    } else {
      json_reply("Reload",
                 "{\"InvalidRequestError\":\"No config file path set\"}", NULL, ds);
    }
  } else if (strcmp(simple, "Stop") == 0) {
    if (server && server->engine && server->engine->stop) {
      server->engine->stop(server->engine->ctx);
    }
    json_reply("Stop", "\"Ok\"", NULL, ds);
  } else if (strcmp(simple, "Exit") == 0) {
    if (server && server->engine && server->engine->stop) {
      server->engine->stop(server->engine->ctx);
    }
    json_reply("Exit", "\"Ok\"", NULL, ds);
  } else if (strcmp(simple, "SetVolume") == 0) {
    if (arg && cJSON_IsNumber(arg)) {
      double vol = arg->valuedouble;
      if (server && server->engine && server->engine->set_fader_volume) {
        double clamped = vol < -150.0 ? -150.0 : (vol > 50.0 ? 50.0 : vol);
        server->engine->set_fader_volume(server->engine->ctx, FADER_MAIN,
                                         clamped, false);
        json_reply("SetVolume", "\"Ok\"", NULL, ds);
      } else {
        json_reply("SetVolume", "\"ProcessingNotRunningError\"", NULL, ds);
      }
    } else {
      json_reply("SetVolume",
                 "{\"InvalidRequestError\":\"Could not parse volume value\"}",
                 NULL, ds);
    }
  } else if (strcmp(simple, "SetMute") == 0) {
    if (arg && cJSON_IsBool(arg)) {
      bool mute = cJSON_IsTrue(arg);
      if (server && server->engine && server->engine->set_fader_mute) {
        server->engine->set_fader_mute(server->engine->ctx, FADER_MAIN, mute);
        json_reply("SetMute", "\"Ok\"", NULL, ds);
      } else {
        json_reply("SetMute", "\"ProcessingNotRunningError\"", NULL, ds);
      }
    } else {
      json_reply("SetMute",
                 "{\"InvalidRequestError\":\"Could not parse mute value\"}",
                 NULL, ds);
    }
  } else if (strcmp(simple, "SetConfigFilePath") == 0) {
    if (arg && cJSON_IsString(arg) && arg->valuestring) {
      const char* path = arg->valuestring;
      if (server && server->engine && server->engine->set_config_path) {
        server->engine->set_config_path(server->engine->ctx, path);
      }
      json_reply("SetConfigFilePath", "\"Ok\"", NULL, ds);
    } else {
      json_reply(
          "SetConfigFilePath",
          "{\"InvalidRequestError\":\"Could not parse Config File Path\"}",
          NULL, ds);
    }
  } else if (strcmp(simple, "SetConfigJson") == 0) {
    if (arg && cJSON_IsString(arg) && arg->valuestring) {
      const char* new_json = arg->valuestring;
      audio_backend_error_t err;
      memset(&err, 0, sizeof(err));
      bool ok =
          server && server->engine && server->engine->set_config_json &&
          server->engine->set_config_json(server->engine->ctx, new_json, &err);
      if (ok) {
        json_reply("SetConfigJson", "\"Ok\"", NULL, ds);
      } else {
        char val[600];
        snprintf(val, sizeof(val), "{\"%s\":\"%s\"}",
                 get_websocket_error_key(err.type), err.message);
        json_reply("SetConfigJson", val, NULL, ds);
      }
    } else {
      json_reply("SetConfigJson",
                 "{\"InvalidRequestError\":\"Could not parse Config JSON\"}",
                 NULL, ds);
    }
  } else if (strcmp(simple, "GetConfigValue") == 0) {
    if (arg && cJSON_IsString(arg) && arg->valuestring) {
      const char* pointer = arg->valuestring;
      char* json = NULL;
      if (server && server->engine && server->engine->get_active_config_json) {
        server->engine->get_active_config_json(server->engine->ctx, &json);
      }
      if (!json) {
        char* path = NULL;
        if (server && server->engine && server->engine->get_config_path) {
          path = server->engine->get_config_path(server->engine->ctx);
        }
        if (path) {
          json = server_read_file_to_string(path);
          free(path);
        }
      }

      char val[2048];
      if (json &&
          server_get_value_at_pointer(json, pointer, val, sizeof(val))) {
        json_reply("GetConfigValue", "\"Ok\"", val, ds);
      } else {
        char err[256];
        snprintf(err, sizeof(err),
                 "{\"InvalidRequestError\":\"Path not found: %s\"}", pointer);
        json_reply("GetConfigValue", err, NULL, ds);
      }
      if (json) free(json);
    } else {
      json_reply("GetConfigValue",
                 "{\"InvalidRequestError\":\"Could not parse pointer\"}", NULL, ds);
    }
  } else if (strcmp(simple, "SetConfigValue") == 0) {
    char pointer[256] = "";
    char* trimmed_val = NULL;
    if (arg && cJSON_IsObject(arg)) {
      cJSON* p_node = cJSON_GetObjectItemCaseSensitive(arg, "pointer");
      cJSON* v_node = cJSON_GetObjectItemCaseSensitive(arg, "value");
      if (p_node && cJSON_IsString(p_node)) {
        strncpy(pointer, p_node->valuestring, sizeof(pointer) - 1);
      }
      if (v_node) {
        trimmed_val = cJSON_PrintUnformatted(v_node);
      }
    }

    if (pointer[0] != '\0' && trimmed_val) {
      char* active_json = NULL;
      if (server && server->engine && server->engine->get_active_config_json) {
        server->engine->get_active_config_json(server->engine->ctx,
                                               &active_json);
      }
      if (!active_json) {
        char* path = NULL;
        if (server && server->engine && server->engine->get_config_path) {
          path = server->engine->get_config_path(server->engine->ctx);
        }
        if (path) {
          active_json = server_read_file_to_string(path);
          free(path);
        }
      }

      if (active_json) {
        char* updated_json =
            server_set_value_at_pointer_str(active_json, pointer, trimmed_val);
        if (updated_json) {
          audio_backend_error_t err;
          memset(&err, 0, sizeof(err));
          bool ok = server && server->engine &&
                    server->engine->set_config_json &&
                    server->engine->set_config_json(server->engine->ctx,
                                                    updated_json, &err);
          if (ok) {
            json_reply("SetConfigValue", "\"Ok\"", NULL, ds);
          } else {
            char val[600];
            snprintf(val, sizeof(val), "{\"%s\":\"%s\"}",
                     get_websocket_error_key(err.type), err.message);
            json_reply("SetConfigValue", val, NULL, ds);
          }
          free(updated_json);
        } else {
          char err[256];
          snprintf(err, sizeof(err),
                   "{\"InvalidRequestError\":\"Path not found: %s\"}", pointer);
          json_reply("SetConfigValue", err, NULL, ds);
        }
        free(active_json);
      } else {
        json_reply("SetConfigValue",
                   "{\"InvalidRequestError\":\"No active config to modify\"}",
                   NULL, ds);
      }
      free(trimmed_val);
    } else {
      json_reply("SetConfigValue",
                 "{\"InvalidRequestError\":\"Could not parse SetConfigValue "
                 "command\"}",
                 NULL, ds);
    }
  } else if (strcmp(simple, "PatchConfig") == 0) {
    if (arg && cJSON_IsObject(arg)) {
      char* active_json = NULL;
      if (server && server->engine && server->engine->get_active_config_json) {
        server->engine->get_active_config_json(server->engine->ctx,
                                               &active_json);
      }
      if (!active_json) {
        char* path = NULL;
        if (server && server->engine && server->engine->get_config_path) {
          path = server->engine->get_config_path(server->engine->ctx);
        }
        if (path) {
          active_json = server_read_file_to_string(path);
          free(path);
        }
      }

      if (active_json) {
        cJSON* target_root = cJSON_Parse(active_json);
        if (target_root) {
          cjson_merge_patch(target_root, arg);
          char* target_json = cJSON_PrintUnformatted(target_root);
          if (target_json) {
            audio_backend_error_t err;
            memset(&err, 0, sizeof(err));
            bool ok = server && server->engine &&
                      server->engine->set_config_json &&
                      server->engine->set_config_json(server->engine->ctx,
                                                      target_json, &err);
            if (ok) {
              json_reply("PatchConfig", "\"Ok\"", NULL, ds);
            } else {
              char val[600];
              snprintf(val, sizeof(val), "{\"%s\":\"%s\"}",
                       get_websocket_error_key(err.type), err.message);
              json_reply("PatchConfig", val, NULL, ds);
            }
            free(target_json);
          } else {
            json_reply(
                "PatchConfig",
                "{\"InvalidRequestError\":\"Failed to format target JSON\"}",
                NULL, ds);
          }
          cJSON_Delete(target_root);
        } else {
          cJSON_Delete(target_root);
          json_reply(
              "PatchConfig",
              "{\"InvalidRequestError\":\"Failed to parse target JSON\"}", NULL, ds);
        }
        free(active_json);
      } else {
        json_reply("PatchConfig",
                   "{\"InvalidRequestError\":\"No active config to patch\"}",
                   NULL, ds);
      }
    } else {
      json_reply(
          "PatchConfig",
          "{\"InvalidRequestError\":\"Could not parse PatchConfig command\"}",
          NULL, ds);
    }
  } else if (strcmp(simple, "GetFaderVolume") == 0) {
    if (arg && cJSON_IsNumber(arg)) {
      int idx = arg->valueint;
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
          json_reply("GetFaderVolume", "\"Ok\"", val, ds);
        } else {
          json_reply("GetFaderVolume", "\"InvalidFaderError\"", NULL, ds);
        }
      } else {
        json_reply("GetFaderVolume", "\"ProcessingNotRunningError\"", NULL, ds);
      }
    } else {
      json_reply("GetFaderVolume",
                 "{\"InvalidRequestError\":\"Could not parse fader index\"}",
                 NULL, ds);
    }
  } else if (strcmp(simple, "SetFaderVolume") == 0 ||
             strcmp(simple, "SetFaderExternalVolume") == 0) {
    int idx = -1;
    double vol = 0.0;
    bool ok = false;
    if (arg && cJSON_IsArray(arg) && cJSON_GetArraySize(arg) >= 2) {
      cJSON* idx_node = cJSON_GetArrayItem(arg, 0);
      cJSON* val_node = cJSON_GetArrayItem(arg, 1);
      if (idx_node && val_node && cJSON_IsNumber(idx_node) &&
          cJSON_IsNumber(val_node)) {
        idx = idx_node->valueint;
        vol = val_node->valuedouble;
        ok = true;
      }
    }
    if (ok) {
      if (server && server->engine && server->engine->set_fader_volume) {
        if (idx >= 0 && idx < FADER_COUNT) {
          double clamped = vol < -150.0 ? -150.0 : (vol > 50.0 ? 50.0 : vol);
          bool instant = (strcmp(simple, "SetFaderExternalVolume") == 0);
          server->engine->set_fader_volume(server->engine->ctx, (fader_t)idx,
                                           clamped, instant);
          json_reply(simple, "\"Ok\"", NULL, ds);
        } else {
          json_reply(simple, "\"InvalidFaderError\"", NULL, ds);
        }
      } else {
        json_reply(simple, "\"ProcessingNotRunningError\"", NULL, ds);
      }
    } else {
      json_reply(simple,
                 "{\"InvalidRequestError\":\"Could not parse "
                 "SetFaderVolume/SetFaderExternalVolume array\"}",
                 NULL, ds);
    }
  } else if (strcmp(simple, "GetFaderMute") == 0) {
    if (arg && cJSON_IsNumber(arg)) {
      int idx = arg->valueint;
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
          json_reply("GetFaderMute", "\"Ok\"", val, ds);
        } else {
          json_reply("GetFaderMute", "\"InvalidFaderError\"", NULL, ds);
        }
      } else {
        json_reply("GetFaderMute", "\"ProcessingNotRunningError\"", NULL, ds);
      }
    } else {
      json_reply("GetFaderMute",
                 "{\"InvalidRequestError\":\"Could not parse fader index\"}",
                 NULL, ds);
    }
  } else if (strcmp(simple, "SetFaderMute") == 0) {
    int idx = -1;
    bool mute = false;
    bool ok = false;
    if (arg && cJSON_IsArray(arg) && cJSON_GetArraySize(arg) >= 2) {
      cJSON* idx_node = cJSON_GetArrayItem(arg, 0);
      cJSON* val_node = cJSON_GetArrayItem(arg, 1);
      if (idx_node && val_node && cJSON_IsNumber(idx_node) &&
          cJSON_IsBool(val_node)) {
        idx = idx_node->valueint;
        mute = cJSON_IsTrue(val_node);
        ok = true;
      }
    }
    if (ok) {
      if (server && server->engine && server->engine->set_fader_mute) {
        if (idx >= 0 && idx < FADER_COUNT) {
          server->engine->set_fader_mute(server->engine->ctx, (fader_t)idx,
                                         mute);
          json_reply("SetFaderMute", "\"Ok\"", NULL, ds);
        } else {
          json_reply("SetFaderMute", "\"InvalidFaderError\"", NULL, ds);
        }
      } else {
        json_reply("SetFaderMute", "\"ProcessingNotRunningError\"", NULL, ds);
      }
    } else {
      json_reply(
          "SetFaderMute",
          "{\"InvalidRequestError\":\"Could not parse SetFaderMute array\"}",
          NULL, ds);
    }
  } else if (strcmp(simple, "ToggleFaderMute") == 0) {
    if (arg && cJSON_IsNumber(arg)) {
      int idx = arg->valueint;
      processing_parameters_t* params = NULL;
      if (server && server->engine &&
          server->engine->get_processing_parameters &&
          server->engine->get_processing_parameters(server->engine->ctx,
                                                    (void**)&params) &&
          params) {
        if (idx >= 0 && idx < FADER_COUNT) {
          bool was_muted =
              processing_parameters_is_muted_for_fader(params, (fader_t)idx);
          if (server->engine->set_fader_mute) {
            server->engine->set_fader_mute(server->engine->ctx, (fader_t)idx,
                                           !was_muted);
          }
          char val[64];
          snprintf(val, sizeof(val), "[%d,%s]", idx,
                   !was_muted ? "true" : "false");
          json_reply("ToggleFaderMute", "\"Ok\"", val, ds);
        } else {
          json_reply("ToggleFaderMute", "\"InvalidFaderError\"", NULL, ds);
        }
      } else {
        json_reply("ToggleFaderMute", "\"ProcessingNotRunningError\"", NULL, ds);
      }
    } else {
      json_reply("ToggleFaderMute",
                 "{\"InvalidRequestError\":\"Could not parse fader index\"}",
                 NULL, ds);
    }
  } else if (strcmp(simple, "GetAvailableCaptureDevices") == 0) {
    if (arg && cJSON_IsString(arg) && arg->valuestring) {
      const char* backend = arg->valuestring;
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
        json_reply("GetAvailableCaptureDevices", "\"Ok\"", val, ds);
      } else {
        json_reply("GetAvailableCaptureDevices", "\"Ok\"", "[]", ds);
      }
    } else {
      json_reply("GetAvailableCaptureDevices",
                 "{\"InvalidRequestError\":\"Could not parse backend\"}", NULL, ds);
    }
  } else if (strcmp(simple, "GetAvailablePlaybackDevices") == 0) {
    if (arg && cJSON_IsString(arg) && arg->valuestring) {
      const char* backend = arg->valuestring;
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
        json_reply("GetAvailablePlaybackDevices", "\"Ok\"", val, ds);
      } else {
        json_reply("GetAvailablePlaybackDevices", "\"Ok\"", "[]", ds);
      }
    } else {
      json_reply("GetAvailablePlaybackDevices",
                 "{\"InvalidRequestError\":\"Could not parse backend\"}", NULL, ds);
    }
  } else if (strcmp(simple, "AdjustVolume") == 0) {
    double delta = 0.0;
    double min_vol = -150.0;
    double max_vol = 50.0;
    bool ok = false;
    if (arg && cJSON_IsNumber(arg)) {
      delta = arg->valuedouble;
      ok = true;
    } else if (arg && cJSON_IsArray(arg)) {
      int size = cJSON_GetArraySize(arg);
      if (size >= 1) {
        cJSON* d_node = cJSON_GetArrayItem(arg, 0);
        if (d_node && cJSON_IsNumber(d_node)) {
          delta = d_node->valuedouble;
          ok = true;
        }
      }
      if (size >= 2) {
        cJSON* min_node = cJSON_GetArrayItem(arg, 1);
        if (min_node && cJSON_IsNumber(min_node))
          min_vol = min_node->valuedouble;
      }
      if (size >= 3) {
        cJSON* max_node = cJSON_GetArrayItem(arg, 2);
        if (max_node && cJSON_IsNumber(max_node))
          max_vol = max_node->valuedouble;
      }
    }

    if (ok) {
      server_handle_adjust_volume_fader(server, FADER_MAIN, delta, min_vol,
                                        max_vol, ds, "AdjustVolume");
    } else {
      json_reply(
          "AdjustVolume",
          "{\"InvalidRequestError\":\"Could not parse AdjustVolume argument\"}",
          NULL, ds);
    }
  } else if (strcmp(simple, "AdjustFaderVolume") == 0) {
    int idx = -1;
    double delta = 0.0;
    double min_vol = -150.0;
    double max_vol = 50.0;
    bool ok = false;
    if (arg && cJSON_IsArray(arg)) {
      int size = cJSON_GetArraySize(arg);
      if (size >= 2) {
        cJSON* idx_node = cJSON_GetArrayItem(arg, 0);
        cJSON* d_node = cJSON_GetArrayItem(arg, 1);
        if (idx_node && d_node && cJSON_IsNumber(idx_node) &&
            cJSON_IsNumber(d_node)) {
          idx = idx_node->valueint;
          delta = d_node->valuedouble;
          ok = true;
        }
      }
      if (size >= 3) {
        cJSON* min_node = cJSON_GetArrayItem(arg, 2);
        if (min_node && cJSON_IsNumber(min_node))
          min_vol = min_node->valuedouble;
      }
      if (size >= 4) {
        cJSON* max_node = cJSON_GetArrayItem(arg, 3);
        if (max_node && cJSON_IsNumber(max_node))
          max_vol = max_node->valuedouble;
      }
    }

    if (ok) {
      if (idx >= 0 && idx < FADER_COUNT) {
        server_handle_adjust_volume_fader(server, (fader_t)idx, delta, min_vol,
                                          max_vol, ds, "AdjustFaderVolume");
      } else {
        json_reply("AdjustFaderVolume", "\"InvalidFaderError\"", NULL, ds);
      }
    } else {
      json_reply("AdjustFaderVolume",
                 "{\"InvalidRequestError\":\"Could not parse AdjustFaderVolume "
                 "array\"}",
                 NULL, ds);
    }
  } else if (strcmp(simple, "GetCaptureDeviceCapabilities") == 0 ||
             strcmp(simple, "GetPlaybackDeviceCapabilities") == 0) {
    char backend[128] = "";
    char device[256] = "";
    bool ok = false;
    if (arg && cJSON_IsArray(arg) && cJSON_GetArraySize(arg) >= 2) {
      cJSON* b_node = cJSON_GetArrayItem(arg, 0);
      cJSON* d_node = cJSON_GetArrayItem(arg, 1);
      if (b_node && d_node && cJSON_IsString(b_node) &&
          cJSON_IsString(d_node)) {
        strncpy(backend, b_node->valuestring, sizeof(backend) - 1);
        strncpy(device, d_node->valuestring, sizeof(device) - 1);
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
          server->engine->get_device_capabilities(
              server->engine->ctx, backend, device, is_capture, &desc, &d_err);
      if (cap_ok && desc) {
        char val[8192];
        format_device_descriptor(desc, val, sizeof(val));
        json_reply(simple, "\"Ok\"", val, ds);
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
        json_reply(simple, err, NULL, ds);
      }
    } else {
      json_reply(simple,
                 "{\"InvalidRequestError\":\"Could not parse backend and "
                 "device arguments\"}",
                 NULL, ds);
    }
  } else if (strcmp(simple, "GetSpectrum") == 0) {
    bool is_capture = true;
    uint32_t channel = 0;
    double min_freq = 20.0;
    double max_freq = 20000.0;
    uint32_t n_bins = 1024;
    bool ok = false;

    if (arg && cJSON_IsObject(arg)) {
      cJSON* item;
      item = cJSON_GetObjectItemCaseSensitive(arg, "is_capture");
      if (item && cJSON_IsBool(item)) is_capture = cJSON_IsTrue(item);
      item = cJSON_GetObjectItemCaseSensitive(arg, "channel");
      if (item && cJSON_IsNumber(item)) channel = (uint32_t)item->valueint;
      item = cJSON_GetObjectItemCaseSensitive(arg, "min_freq");
      if (item && cJSON_IsNumber(item)) min_freq = item->valuedouble;
      item = cJSON_GetObjectItemCaseSensitive(arg, "max_freq");
      if (item && cJSON_IsNumber(item)) max_freq = item->valuedouble;
      item = cJSON_GetObjectItemCaseSensitive(arg, "n_bins");
      if (item && cJSON_IsNumber(item)) n_bins = (uint32_t)item->valueint;
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
          json_reply("GetSpectrum", "\"Ok\"", spec_buf, ds);
          free(spec_buf);
        } else {
          json_reply("GetSpectrum", "{\"DeviceError\":\"Out of memory\"}", NULL, ds);
        }
        if (spec.frequencies) free(spec.frequencies);
        if (spec.magnitudes) free(spec.magnitudes);
      } else {
        json_reply("GetSpectrum",
                   "{\"DeviceError\":\"Failed to compute spectrum\"}", NULL, ds);
      }
    } else {
      json_reply(
          "GetSpectrum",
          "{\"InvalidRequestError\":\"Could not parse GetSpectrum arguments\"}",
          NULL, ds);
    }
  } else if (strcmp(simple, "ReadConfigJson") == 0) {
    if (arg && cJSON_IsString(arg) && arg->valuestring) {
      const char* config_json = arg->valuestring;
      dsp_config_t* parsed = NULL;
      config_error_t cerr;
      memset(&cerr, 0, sizeof(cerr));
      if (config_loader_parse(config_json, &parsed, &cerr) == 0 && parsed) {
        json_reply("ReadConfigJson", "\"Ok\"", config_json, ds);
        dsp_config_free(parsed);
      } else {
        char val[600];
        snprintf(val, sizeof(val), "{\"ConfigValidationError\":\"%s\"}",
                 cerr.message);
        json_reply("ReadConfigJson", val, NULL, ds);
      }
    } else {
      json_reply(
          "ReadConfigJson",
          "{\"InvalidRequestError\":\"Could not parse input config JSON\"}",
          NULL, ds);
    }
  } else if (strcmp(simple, "ValidateConfigJson") == 0) {
    if (arg && cJSON_IsString(arg) && arg->valuestring) {
      const char* config_json = arg->valuestring;
      dsp_config_t* parsed = NULL;
      config_error_t cerr;
      memset(&cerr, 0, sizeof(cerr));
      if (config_loader_parse(config_json, &parsed, &cerr) == 0 && parsed) {
        json_reply("ValidateConfigJson", "\"Ok\"", NULL, ds);
        dsp_config_free(parsed);
      } else {
        char val[600];
        snprintf(val, sizeof(val), "{\"ConfigValidationError\":\"%s\"}",
                 cerr.message);
        json_reply("ValidateConfigJson", val, NULL, ds);
      }
    } else {
      json_reply(
          "ValidateConfigJson",
          "{\"InvalidRequestError\":\"Could not parse input config JSON\"}",
          NULL, ds);
    }
  } else {
    dyn_string_printf(ds, "{\"Invalid\":{\"error\":\"Unsupported command\"}}");
  }
  pthread_mutex_unlock(&server->sessions_mutex);
  cJSON_Delete(root);
}

/**
 * @brief Sends a WebSocket text frame to a client file descriptor.
 *
 * Packages the payload into a standard RFC 6455 WebSocket text frame (opcode
 * 0x81), handles payload length fields (up to 64-bit lengths), and writes to
 * the socket.
 *
 * @param fd The client socket file descriptor.
 * @param response The null-terminated payload string to send.
 */
static void send_websocket_frame(socket_t fd, const char* response) {
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
    send(fd, frame, (int)(header_len + resp_len), 0);
  } else {
    char* dyn_frame = (char*)malloc(header_len + resp_len);
    if (dyn_frame) {
      dyn_frame[0] = (char)0x81;
      dyn_frame[1] = frame[1];
      memcpy(&dyn_frame[2], &frame[2], header_len - 2);
      memcpy(&dyn_frame[header_len], response, resp_len);
      send(fd, dyn_frame, (int)(header_len + resp_len), 0);
      free(dyn_frame);
    }
  }
}

/**
 * @brief Thread entry point for the WebSocket server.
 *
 * Handles the polling loop for accepting new client connections, receiving
 * WebSocket frames, decoding masking key, executing commands, and pushing
 * periodic updates (VU levels, signal levels, spectrum) to subscribed clients.
 *
 * @param arg Pointer to the websocket_server_t structure.
 * @return Always NULL.
 */
static void* server_thread_func(void* arg) {
  websocket_server_t* server = (websocket_server_t*)arg;
  socket_t client_fds[32];
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
    int ret = poll_sockets(fds, num_clients + 1, 50);

    pthread_mutex_lock(&server->sessions_mutex);

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
          if (current_cap_peak && current_cap_rms) {
            processing_parameters_get_capture_signal_peak(
                params, current_cap_peak, cap_channels);
            processing_parameters_get_capture_signal_rms(params, current_cap_rms,
                                                         cap_channels);

            level_history_append(&server->capture_peak_history, current_cap_peak,
                                 cap_channels, now);
            level_history_append(&server->capture_rms_history, current_cap_rms,
                                 cap_channels, now);

            if (server->capture_global_peaks_count != cap_channels) {
              double* new_peaks = (double*)realloc(
                  server->capture_global_peaks, cap_channels * sizeof(double));
              if (new_peaks) {
                server->capture_global_peaks = new_peaks;
                for (size_t k = server->capture_global_peaks_count;
                     k < cap_channels; k++) {
                  server->capture_global_peaks[k] = -1000.0;
                }
                server->capture_global_peaks_count = cap_channels;
              }
            }
            size_t limit = cap_channels < server->capture_global_peaks_count
                               ? cap_channels
                               : server->capture_global_peaks_count;
            for (size_t k = 0; k < limit; k++) {
              if (server->capture_global_peaks &&
                  current_cap_peak[k] > server->capture_global_peaks[k]) {
                server->capture_global_peaks[k] = current_cap_peak[k];
              }
            }
          }
        }

        if (pb_channels > 0) {
          current_pb_peak = (double*)malloc(pb_channels * sizeof(double));
          current_pb_rms = (double*)malloc(pb_channels * sizeof(double));
          if (current_pb_peak && current_pb_rms) {
            processing_parameters_get_playback_signal_peak(
                params, current_pb_peak, pb_channels);
            processing_parameters_get_playback_signal_rms(params, current_pb_rms,
                                                         pb_channels);

            level_history_append(&server->playback_peak_history, current_pb_peak,
                                 pb_channels, now);
            level_history_append(&server->playback_rms_history, current_pb_rms,
                                 pb_channels, now);

            if (server->playback_global_peaks_count != pb_channels) {
              double* new_peaks = (double*)realloc(
                  server->playback_global_peaks, pb_channels * sizeof(double));
              if (new_peaks) {
                server->playback_global_peaks = new_peaks;
                for (size_t k = server->playback_global_peaks_count;
                     k < pb_channels; k++) {
                  server->playback_global_peaks[k] = -1000.0;
                }
                server->playback_global_peaks_count = pb_channels;
              }
            }
            size_t limit = pb_channels < server->playback_global_peaks_count
                               ? pb_channels
                               : server->playback_global_peaks_count;
            for (size_t k = 0; k < limit; k++) {
              if (server->playback_global_peaks &&
                  current_pb_peak[k] > server->playback_global_peaks[k]) {
                server->playback_global_peaks[k] = current_pb_peak[k];
              }
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
              double* new_rms = (double*)malloc(pb_channels * sizeof(double));
              double* new_peak = (double*)malloc(pb_channels * sizeof(double));
              if (new_rms && new_peak) {
                size_t copy_count = session->vu_pb_channels < pb_channels
                                        ? session->vu_pb_channels
                                        : pb_channels;
                if (session->vu_pb_rms) {
                  memcpy(new_rms, session->vu_pb_rms,
                         copy_count * sizeof(double));
                  free(session->vu_pb_rms);
                }
                if (session->vu_pb_peak) {
                  memcpy(new_peak, session->vu_pb_peak,
                         copy_count * sizeof(double));
                  free(session->vu_pb_peak);
                }
                for (size_t k = copy_count; k < pb_channels; k++) {
                  new_rms[k] = current_pb_rms[k];
                  new_peak[k] = current_pb_peak[k];
                }
                session->vu_pb_rms = new_rms;
                session->vu_pb_peak = new_peak;
                session->vu_pb_channels = pb_channels;
              } else {
                if (new_rms) free(new_rms);
                if (new_peak) free(new_peak);
              }
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
                double* new_rms =
                    (double*)malloc(cap_channels * sizeof(double));
                double* new_peak =
                    (double*)malloc(cap_channels * sizeof(double));
                if (new_rms && new_peak) {
                  size_t copy_count = session->vu_cap_channels < cap_channels
                                          ? session->vu_cap_channels
                                          : cap_channels;
                  if (session->vu_cap_rms) {
                    memcpy(new_rms, session->vu_cap_rms,
                           copy_count * sizeof(double));
                    free(session->vu_cap_rms);
                  }
                  if (session->vu_cap_peak) {
                    memcpy(new_peak, session->vu_cap_peak,
                           copy_count * sizeof(double));
                    free(session->vu_cap_peak);
                  }
                  for (size_t k = copy_count; k < cap_channels; k++) {
                    new_rms[k] = current_cap_rms[k];
                    new_peak[k] = current_cap_peak[k];
                  }
                  session->vu_cap_rms = new_rms;
                  session->vu_cap_peak = new_peak;
                  session->vu_cap_channels = cap_channels;
                } else {
                  if (new_rms) free(new_rms);
                  if (new_peak) free(new_peak);
                }
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

            if (p_rms_str && p_pk_str && c_rms_str && c_pk_str) {
              format_double_array(session->vu_pb_rms, pb_channels, p_rms_str,
                                  pb_channels * 30 + 10);
              format_double_array(session->vu_pb_peak, pb_channels, p_pk_str,
                                  pb_channels * 30 + 10);
              format_double_array(session->vu_cap_rms, cap_channels, c_rms_str,
                                  cap_channels * 30 + 10);
              format_double_array(session->vu_cap_peak, cap_channels, c_pk_str,
                                  cap_channels * 30 + 10);

              char* msg = (char*)malloc((pb_channels + cap_channels) * 120 + 200);
              if (msg) {
                sprintf(msg,
                        "{\"VuLevelsEvent\":{\"result\":\"Ok\",\"value\":{"
                        "\"playback_rms\":%s,\"playback_peak\":%s,\"capture_rms\":%"
                        "s,\"capture_peak\":%s}}}",
                        p_rms_str, p_pk_str, c_rms_str, c_pk_str);
                send_websocket_frame(client_fds[i], msg);
                free(msg);
              }
            }

            if (p_rms_str) free(p_rms_str);
            if (p_pk_str) free(p_pk_str);
            if (c_rms_str) free(c_rms_str);
            if (c_pk_str) free(c_pk_str);
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
            if (rms_str && pk_str) {
              format_double_array(current_pb_rms, pb_channels, rms_str,
                                  pb_channels * 30 + 10);
              format_double_array(current_pb_peak, pb_channels, pk_str,
                                  pb_channels * 30 + 10);

              char* msg = (char*)malloc(pb_channels * 100 + 200);
              if (msg) {
                sprintf(msg,
                        "{\"SignalLevelsEvent\":{\"result\":\"Ok\",\"value\":{"
                        "\"side\":\"playback\",\"rms\":%s,\"peak\":%s}}}",
                        rms_str, pk_str);
                send_websocket_frame(client_fds[i], msg);
                free(msg);
              }
            }
            if (rms_str) free(rms_str);
            if (pk_str) free(pk_str);
          }
          if (send_cap && cap_channels > 0) {
            char* rms_str = (char*)malloc(cap_channels * 30 + 10);
            char* pk_str = (char*)malloc(cap_channels * 30 + 10);
            if (rms_str && pk_str) {
              format_double_array(current_cap_rms, cap_channels, rms_str,
                                  cap_channels * 30 + 10);
              format_double_array(current_cap_peak, cap_channels, pk_str,
                                  cap_channels * 30 + 10);

              char* msg = (char*)malloc(cap_channels * 100 + 200);
              if (msg) {
                sprintf(msg,
                        "{\"SignalLevelsEvent\":{\"result\":\"Ok\",\"value\":{"
                        "\"side\":\"capture\",\"rms\":%s,\"peak\":%s}}}",
                        rms_str, pk_str);
                send_websocket_frame(client_fds[i], msg);
                free(msg);
              }
            }
            if (rms_str) free(rms_str);
            if (pk_str) free(pk_str);
          }
        }

        if (session->spectrum_subscribed) {
          double interval = session->spectrum_max_rate > 0.0
                                ? 1000.0 / session->spectrum_max_rate
                                : 0.0;
          if (now - session->last_spectrum_push_time >= interval) {
            spectrum_t spec;
            memset(&spec, 0, sizeof(spec));
            bool spec_ok =
                server && server->engine && server->engine->get_spectrum &&
                server->engine->get_spectrum(
                    server->engine->ctx, session->spectrum_is_capture,
                    session->spectrum_channel, session->spectrum_min_freq,
                    session->spectrum_max_freq, session->spectrum_n_bins,
                    &spec);
            if (spec_ok) {
              size_t spec_buf_size = spec.count * 50 + 200;
              char* spec_buf = (char*)malloc(spec_buf_size);
              if (spec_buf) {
                format_spectrum(&spec, spec_buf, spec_buf_size);
                char* msg = (char*)malloc(spec_buf_size + 120);
                if (msg) {
                  sprintf(msg,
                          "{\"SpectrumEvent\":{\"result\":\"Ok\",\"value\":%s}}",
                          spec_buf);
                  send_websocket_frame(client_fds[i], msg);
                  free(msg);
                }
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
    pthread_mutex_unlock(&server->sessions_mutex);

    if (ret > 0) {
      if (fds[0].revents & POLLIN) {
        socket_t cfd = accept(server->server_fd, NULL, NULL);
        if (!IS_INVALID_SOCKET(cfd) && num_clients < 32) {
          client_fds[num_clients] = cfd;
          last_state[num_clients][0] = '\0';

          pthread_mutex_lock(&server->sessions_mutex);
          client_session_t* session = &server->client_sessions[num_clients];
          memset(session, 0, sizeof(client_session_t));
          uint64_t now_ms = get_time_ms();
          session->last_cap_peak_time = now_ms;
          session->last_cap_rms_time = now_ms;
          session->last_pb_peak_time = now_ms;
          session->last_pb_rms_time = now_ms;
          pthread_mutex_unlock(&server->sessions_mutex);

          num_clients++;
        } else if (!IS_INVALID_SOCKET(cfd)) {
          CLOSE_SOCKET(cfd);
        }
      }
      for (int i = 0; i < num_clients; i++) {
        if (fds[i + 1].revents & (POLLIN | POLLERR | POLLHUP)) {
          char buf[4096];
          int n = recv(client_fds[i], buf, sizeof(buf) - 1, 0);
          if (n <= 0) {
            CLOSE_SOCKET(client_fds[i]);
            pthread_mutex_lock(&server->sessions_mutex);
            client_session_clear(&server->client_sessions[i]);

            for (int j = i; j < num_clients - 1; j++) {
              client_fds[j] = client_fds[j + 1];
              strcpy(last_state[j], last_state[j + 1]);
              server->client_sessions[j] = server->client_sessions[j + 1];
            }
            memset(&server->client_sessions[num_clients - 1], 0,
                   sizeof(client_session_t));
            pthread_mutex_unlock(&server->sessions_mutex);
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
                send(client_fds[i], reply, (int)strlen(reply), 0);
              }
              continue;
            }

            int offset = 0;
            while (offset < n) {
              unsigned char first_byte = (unsigned char)buf[offset];
              if (first_byte == 0x81 || (first_byte & 0x0F) == 0x08) {
                if ((first_byte & 0x0F) == 0x08) {
                  CLOSE_SOCKET(client_fds[i]);
                  pthread_mutex_lock(&server->sessions_mutex);
                  client_session_clear(&server->client_sessions[i]);
                  for (int j = i; j < num_clients - 1; j++) {
                    client_fds[j] = client_fds[j + 1];
                    strcpy(last_state[j], last_state[j + 1]);
                    server->client_sessions[j] = server->client_sessions[j + 1];
                  }
                  memset(&server->client_sessions[num_clients - 1], 0,
                         sizeof(client_session_t));
                  pthread_mutex_unlock(&server->sessions_mutex);
                  num_clients--;
                  i--;
                  break;
                }
                if (offset + 2 > n) break;
                unsigned char len_byte = (unsigned char)buf[offset + 1];
                size_t payload_len = len_byte & 0x7F;
                int mask_offset = 2;
                if (payload_len == 126) {
                  if (offset + 4 > n) break;
                  payload_len = ((unsigned char)buf[offset + 2] << 8) |
                                (unsigned char)buf[offset + 3];
                  mask_offset = 4;
                } else if (payload_len == 127) {
                  if (offset + 10 > n) break;
                  payload_len = 0;
                  for (int j = 0; j < 8; j++) {
                    payload_len =
                        (payload_len << 8) | (unsigned char)buf[offset + 2 + j];
                  }
                  mask_offset = 10;
                }
                if (payload_len > 4096 || offset + mask_offset + 4 + payload_len > (size_t)n) {
                  // Payload size exceeds buffer limits or packet bounds. Close socket.
                  CLOSE_SOCKET(client_fds[i]);
                  pthread_mutex_lock(&server->sessions_mutex);
                  client_session_clear(&server->client_sessions[i]);

                  for (int j = i; j < num_clients - 1; j++) {
                    client_fds[j] = client_fds[j + 1];
                    strcpy(last_state[j], last_state[j + 1]);
                    server->client_sessions[j] = server->client_sessions[j + 1];
                  }
                  memset(&server->client_sessions[num_clients - 1], 0,
                         sizeof(client_session_t));
                  pthread_mutex_unlock(&server->sessions_mutex);
                  num_clients--;
                  i--;
                  break;
                }

                unsigned char* mask =
                    (unsigned char*)&buf[offset + mask_offset];
                char* payload = &buf[offset + mask_offset + 4];
                for (size_t p = 0; p < payload_len; p++) {
                  payload[p] ^= mask[p % 4];
                }
                char saved_char = payload[payload_len];
                payload[payload_len] = '\0';

                logger_debug(&server_logger, "Received WS frame: %s",
                             log_arg_string(payload), log_arg_none(),
                             log_arg_none(), log_arg_none());

                dyn_string_t ds;
                dyn_string_init(&ds, 4096);
                websocket_server_handle_command(server, i, payload, &ds);

                payload[payload_len] = saved_char;

                if (ds.data && ds.data[0] != '\0') {
                  logger_debug(&server_logger, "Sending WS response: %s",
                               log_arg_string(ds.data), log_arg_none(),
                               log_arg_none(), log_arg_none());
                  send_websocket_frame(client_fds[i], ds.data);
                }
                dyn_string_free(&ds);
                offset += mask_offset + 4 + payload_len;
              } else {
                logger_debug(&server_logger, "Received raw TCP: %s",
                             log_arg_string(&buf[offset]), log_arg_none(),
                             log_arg_none(), log_arg_none());

                dyn_string_t ds;
                dyn_string_init(&ds, 4096);
                websocket_server_handle_command(server, i, &buf[offset], &ds);
                if (ds.data && ds.data[0] != '\0') {
                  logger_debug(&server_logger, "Sending raw TCP response: %s",
                               log_arg_string(ds.data), log_arg_none(),
                               log_arg_none(), log_arg_none());
                  send(client_fds[i], ds.data, (int)strlen(ds.data), 0);
                }
                dyn_string_free(&ds);
                break;
              }
            }
          }
        }
      }
    }
  }
  for (int i = 0; i < num_clients; i++) {
    CLOSE_SOCKET(client_fds[i]);
  }
  return NULL;
}

/// Start the WebSocket server listening and processing connections.
bool websocket_server_start(websocket_server_t* server) {
  if (!server || atomic_load_explicit(&server->running, memory_order_acquire))
    return false;

#ifdef _WIN32
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    return false;
  }
#endif

  server->server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (IS_INVALID_SOCKET(server->server_fd)) {
#ifdef _WIN32
    WSACleanup();
#endif
    return false;
  }

  int opt = 1;
#ifdef _WIN32
  setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt,
             sizeof(opt));
#else
  setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(server->port);
  inet_pton(AF_INET, server->host, &addr.sin_addr);

  if (IS_SOCKET_ERROR(
          bind(server->server_fd, (struct sockaddr*)&addr, sizeof(addr)))) {
    CLOSE_SOCKET(server->server_fd);
    server->server_fd = INVALID_SOCKET_VAL;
#ifdef _WIN32
    WSACleanup();
#endif
    return false;
  }

  if (IS_SOCKET_ERROR(listen(server->server_fd, 10))) {
    CLOSE_SOCKET(server->server_fd);
    server->server_fd = INVALID_SOCKET_VAL;
#ifdef _WIN32
    WSACleanup();
#endif
    return false;
  }

  atomic_store_explicit(&server->running, true, memory_order_release);
  if (pthread_create(&server->thread, NULL, server_thread_func, server) != 0) {
    atomic_store_explicit(&server->running, false, memory_order_release);
    CLOSE_SOCKET(server->server_fd);
    server->server_fd = INVALID_SOCKET_VAL;
#ifdef _WIN32
    WSACleanup();
#endif
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
  if (!IS_INVALID_SOCKET(server->server_fd)) {
    CLOSE_SOCKET(server->server_fd);
    server->server_fd = INVALID_SOCKET_VAL;
  }
#ifdef _WIN32
  WSACleanup();
#endif
}

/// Destroy and free the WebSocket server.
void websocket_server_free(websocket_server_t* server) {
  if (!server) return;
  websocket_server_stop(server);

  // Free level history arrays
  level_history_clear(&server->capture_peak_history);
  level_history_clear(&server->capture_rms_history);
  level_history_clear(&server->playback_peak_history);
  level_history_clear(&server->playback_rms_history);

  // Free global peak arrays
  if (server->capture_global_peaks) free(server->capture_global_peaks);
  if (server->playback_global_peaks) free(server->playback_global_peaks);

  // Free client sessions
  for (size_t i = 0; i < 32; i++) {
    client_session_clear(&server->client_sessions[i]);
  }

  pthread_mutex_destroy(&server->sessions_mutex);
  free(server);
}

bool websocket_server_get_client_vu_subscribed(const websocket_server_t* server,
                                               int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32) return false;
  pthread_mutex_lock((pthread_mutex_t*)&server->sessions_mutex);
  bool res = server->client_sessions[client_idx].vu_subscribed;
  pthread_mutex_unlock((pthread_mutex_t*)&server->sessions_mutex);
  return res;
}

double websocket_server_get_client_vu_max_rate(const websocket_server_t* server,
                                               int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32) return 0.0;
  pthread_mutex_lock((pthread_mutex_t*)&server->sessions_mutex);
  double res = server->client_sessions[client_idx].vu_max_rate;
  pthread_mutex_unlock((pthread_mutex_t*)&server->sessions_mutex);
  return res;
}

double websocket_server_get_client_vu_attack(const websocket_server_t* server,
                                             int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32) return 0.0;
  pthread_mutex_lock((pthread_mutex_t*)&server->sessions_mutex);
  double res = server->client_sessions[client_idx].vu_attack;
  pthread_mutex_unlock((pthread_mutex_t*)&server->sessions_mutex);
  return res;
}

double websocket_server_get_client_vu_release(const websocket_server_t* server,
                                              int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32) return 0.0;
  pthread_mutex_lock((pthread_mutex_t*)&server->sessions_mutex);
  double res = server->client_sessions[client_idx].vu_release;
  pthread_mutex_unlock((pthread_mutex_t*)&server->sessions_mutex);
  return res;
}

void websocket_server_set_client_vu_subscribed(websocket_server_t* server,
                                               int client_idx,
                                               bool subscribed) {
  if (!server || client_idx < 0 || client_idx >= 32) return;
  pthread_mutex_lock(&server->sessions_mutex);
  server->client_sessions[client_idx].vu_subscribed = subscribed;
  pthread_mutex_unlock(&server->sessions_mutex);
}
