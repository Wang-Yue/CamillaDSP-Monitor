#if defined(ENABLE_BLUEZ)

#include "bluez_backend.h"

#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "Logging/app_logger.h"

struct bluez_capture {
  int pipe_fd;
  int ctrl_fd;
  int channels;
  binary_sample_format_t format;
  int sample_rate;
  int chunk_size;
  char service[256];
  char dbus_path[512];
  uint8_t* raw_buf;
  size_t raw_buf_capacity;
  bool active;
  logger_t logger;
};

/**
 * @brief Helper function to get the size in bytes of a sample format.
 *
 * @param format The binary sample format.
 * @return The size of one sample in bytes, or 0 if format is unknown.
 */
static size_t get_sample_size(binary_sample_format_t format) {
  switch (format) {
    case BINARY_SAMPLE_FORMAT_S16_LE:
      return 2;
    case BINARY_SAMPLE_FORMAT_S24_3_LE:
      return 3;
    case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
      return 4;
    case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
      return 4;
    case BINARY_SAMPLE_FORMAT_S32_LE:
      return 4;
    case BINARY_SAMPLE_FORMAT_F32_LE:
      return 4;
    case BINARY_SAMPLE_FORMAT_F64_LE:
      return 8;
    default:
      return 0;
  }
}

/**
 * @brief Helper function to decode a raw sample of a given format into a
 * double.
 *
 * Decodes little-endian samples of various formats, scaling them to the range
 * [-1.0, 1.0].
 *
 * @param src Pointer to the raw bytes of the sample.
 * @param format The binary sample format.
 * @return The decoded sample as a double.
 */
static inline double decode_sample(const uint8_t* src,
                                   binary_sample_format_t format) {
  switch (format) {
    case BINARY_SAMPLE_FORMAT_S16_LE: {
      int16_t val = src[0] | (src[1] << 8);
      return (double)val / 32768.0;
    }
    case BINARY_SAMPLE_FORMAT_S24_3_LE: {
      int32_t val = src[0] | (src[1] << 8) | (src[2] << 16);
      if (val & 0x800000) val |= ~0xFFFFFF;
      return (double)val / 8388608.0;
    }
    case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE: {
      int32_t val = src[0] | (src[1] << 8) | (src[2] << 16);
      if (val & 0x800000) val |= ~0xFFFFFF;
      return (double)val / 8388608.0;
    }
    case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE: {
      int32_t val = (src[1] << 8) | (src[2] << 16) | (src[3] << 24);
      return (double)val / 2147483648.0;
    }
    case BINARY_SAMPLE_FORMAT_S32_LE: {
      int32_t val = src[0] | (src[1] << 8) | (src[2] << 16) | (src[3] << 24);
      return (double)val / 2147483648.0;
    }
    case BINARY_SAMPLE_FORMAT_F32_LE: {
      float val;
      memcpy(&val, src, 4);
      return (double)val;
    }
    case BINARY_SAMPLE_FORMAT_F64_LE: {
      double val;
      memcpy(&val, src, 8);
      return val;
    }
    default:
      return 0.0;
  }
}

// --- Capture Backend VTable Adapters ---
// The following static functions adapt the bluez_capture_t interface
// to the generic capture_backend_vtable_t callback structure.

/**
 * @brief VTable callback to destroy the backend context.
 */
static void vtable_bluez_destroy(void* ctx) {
  bluez_capture_destroy((bluez_capture_t*)ctx);
}

/**
 * @brief VTable callback to open the backend device.
 */
static bool vtable_bluez_open(void* ctx, backend_error_t* err) {
  return bluez_capture_open((bluez_capture_t*)ctx, err);
}

/**
 * @brief VTable callback to read audio data.
 */
static bool vtable_bluez_read(void* ctx, size_t frames, audio_chunk_t* chunk,
                              backend_error_t* err) {
  return bluez_capture_read((bluez_capture_t*)ctx, frames, chunk, err);
}

/**
 * @brief VTable callback to close the backend device.
 */
static void vtable_bluez_close(void* ctx) {
  bluez_capture_close((bluez_capture_t*)ctx);
}

/**
 * @brief VTable callback to check if there is a pending sample rate change
 * request.
 */
static bool vtable_bluez_get_pending_rate_change(void* ctx, double* out_rate) {
  (void)ctx;
  (void)out_rate;
  return false;
}

/**
 * @brief VTable callback to check if pitch control is supported.
 */
static bool vtable_bluez_pitch_supported(void* ctx) {
  (void)ctx;
  return false;
}

/**
 * @brief VTable callback to set the pitch multiplier.
 */
static void vtable_bluez_set_pitch(void* ctx, double multiplier) {
  (void)ctx;
  (void)multiplier;
}

/**
 * @brief VTable callback to wait for audio data to become available (polling).
 */
static bool vtable_bluez_wait(void* ctx, uint32_t timeout_ms) {
  bluez_capture_t* capture = (bluez_capture_t*)ctx;
  if (!capture->active || capture->pipe_fd == -1) return false;
  struct pollfd pfd;
  pfd.fd = capture->pipe_fd;
  pfd.events = POLLIN;
  int res = poll(&pfd, 1, (int)timeout_ms);
  return (res > 0 && (pfd.revents & POLLIN));
}

/**
 * @brief VTable callback to pause/unpause capture.
 */
static void vtable_bluez_set_paused(void* ctx, bool paused) {
  (void)ctx;
  (void)paused;
}

static const capture_backend_vtable_t BLUEZ_CAPTURE_VTABLE = {
    .open = vtable_bluez_open,
    .read = vtable_bluez_read,
    .close = vtable_bluez_close,
    .get_pending_rate_change = vtable_bluez_get_pending_rate_change,
    .is_pitch_control_supported = vtable_bluez_pitch_supported,
    .set_pitch = vtable_bluez_set_pitch,
    .wait_for_data = vtable_bluez_wait,
    .set_is_paused = vtable_bluez_set_paused,
    .destroy = vtable_bluez_destroy};

capture_backend_t* bluez_capture_create(const capture_device_config_t* config,
                                        int sample_rate, int chunk_size,
                                        processing_parameters_t* params,
                                        backend_error_t* err) {
  (void)params;
  bluez_capture_t* capture =
      (bluez_capture_t*)calloc(1, sizeof(bluez_capture_t));
  if (!capture) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Memory allocation failed");
    return NULL;
  }

  capture->logger = logger_create("dsp.capture.bluez");
  capture->channels = config->cfg.bluez.channels;
  capture->format = config->cfg.bluez.format;
  capture->sample_rate = sample_rate;
  capture->chunk_size = chunk_size;
  capture->pipe_fd = -1;
  capture->ctrl_fd = -1;
  capture->active = false;

  if (config->cfg.bluez.has_service && strlen(config->cfg.bluez.service) > 0) {
    snprintf(capture->service, sizeof(capture->service), "%s",
             config->cfg.bluez.service);
  } else {
    snprintf(capture->service, sizeof(capture->service), "org.bluealsa");
  }

  if (config->cfg.bluez.has_dbus_path &&
      strlen(config->cfg.bluez.dbus_path) > 0) {
    snprintf(capture->dbus_path, sizeof(capture->dbus_path), "%s",
             config->cfg.bluez.dbus_path);
  } else {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Missing dbus_path for Bluez backend");
    free(capture);
    return NULL;
  }

  capture_backend_t* backend =
      (capture_backend_t*)malloc(sizeof(capture_backend_t));
  backend->ctx = capture;
  backend->vtable = &BLUEZ_CAPTURE_VTABLE;
  return backend;
}

bool bluez_capture_open(bluez_capture_t* capture, backend_error_t* err) {
  DBusError derr;
  dbus_error_init(&derr);

  // Connect to the DBus system bus where BlueALSA exposes its services.
  DBusConnection* conn = dbus_bus_get(DBUS_BUS_SYSTEM, &derr);
  if (!conn) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "Failed to connect to System DBus: %s",
               derr.message);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    dbus_error_free(&derr);
    return false;
  }

  // Send a synchronous method call to BlueALSA to request opening the PCM
  // stream.
  DBusMessage* msg = dbus_message_new_method_call(
      capture->service, capture->dbus_path, "org.bluealsa.PCM1", "Open");
  if (!msg) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create DBus Open message");
    return false;
  }

  DBusMessage* reply =
      dbus_connection_send_with_reply_and_block(conn, msg, -1, &derr);
  dbus_message_unref(msg);

  if (!reply) {
    if (err) {
      char msg_buf[256];
      snprintf(msg_buf, sizeof(msg_buf), "Bluez DBus Open call failed: %s",
               derr.message);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg_buf);
    }
    dbus_error_free(&derr);
    return false;
  }

  // Extract the returned Unix file descriptors (data pipe and control socket)
  // from the DBus response.
  int pipe_fd = -1;
  int ctrl_fd = -1;
  DBusMessageIter iter;
  if (dbus_message_iter_init(reply, &iter)) {
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UNIX_FD) {
      dbus_message_iter_get_basic(&iter, &pipe_fd);
    }
    if (dbus_message_iter_next(&iter) &&
        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UNIX_FD) {
      dbus_message_iter_get_basic(&iter, &ctrl_fd);
    }
  }

  if (pipe_fd == -1) {
    dbus_message_unref(reply);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Open reply did not contain valid UNIX FDs");
    return false;
  }

  // Duplicate FDs so we own them independently of the DBus message lifecycle.
  capture->pipe_fd = dup(pipe_fd);
  capture->ctrl_fd = (ctrl_fd != -1) ? dup(ctrl_fd) : -1;

  dbus_message_unref(reply);

  if (capture->pipe_fd < 0) {
    if (capture->ctrl_fd >= 0) {
      close(capture->ctrl_fd);
      capture->ctrl_fd = -1;
    }
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to duplicate pipe FD");
    return false;
  }

  // Set O_NONBLOCK to prevent read operations from hanging the thread if there
  // is a delay.
  int flags = fcntl(capture->pipe_fd, F_GETFL, 0);
  fcntl(capture->pipe_fd, F_SETFL, flags | O_NONBLOCK);

  capture->active = true;
  return true;
}

bool bluez_capture_read(bluez_capture_t* capture, size_t frames,
                        audio_chunk_t* chunk, backend_error_t* err) {
  if (audio_chunk_get_channels(chunk) < (size_t)capture->channels) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INVALID_CHANNELS,
                         "Chunk channels count does not match capture channels");
    }
    return false;
  }
  if (!capture->active || capture->pipe_fd == -1) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Bluez capture not active");
    return false;
  }

  // Calculate the exact number of bytes needed for the requested number of
  // frames.
  size_t sample_size = get_sample_size(capture->format);
  size_t required_bytes = frames * capture->channels * sample_size;

  // Grow the temporary raw read buffer if the current capacity is insufficient.
  if (capture->raw_buf_capacity < required_bytes) {
    uint8_t* new_buf = (uint8_t*)realloc(capture->raw_buf, required_bytes);
    if (!new_buf) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                           "Failed to reallocate BlueZ capture buffer");
      return false;
    }
    capture->raw_buf = new_buf;
    capture->raw_buf_capacity = required_bytes;
  }

  // Read raw bytes from the non-blocking pipe. Since it's non-blocking, we loop
  // and use poll to wait for the data to become available.
  size_t bytes_read = 0;
  while (bytes_read < required_bytes) {
    struct pollfd pfd;
    pfd.fd = capture->pipe_fd;
    pfd.events = POLLIN;
    int res = poll(&pfd, 1, 1000);
    if (res < 0) {
      if (errno == EINTR) continue;  // Interrupted by signal, retry.
      if (err)
        backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                           "Bluez read poll failed");
      return false;
    }
    if (res == 0) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_READ_ERROR, "Bluez read timeout");
      return false;
    }

    ssize_t n = read(capture->pipe_fd, capture->raw_buf + bytes_read,
                     required_bytes - bytes_read);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;  // No data available yet, poll again.
      }
      if (err)
        backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                           "Bluez read pipe failed");
      return false;
    }
    if (n == 0) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_READ_ERROR, "Bluez stream EOF");
      return false;
    }
    bytes_read += (size_t)n;
  }

  // Decode the raw interleaved bytes into non-interleaved float/double channels
  // within the destination audio chunk.
  for (size_t f = 0; f < frames; f++) {
    for (int c = 0; c < capture->channels; c++) {
      size_t offset = (f * capture->channels + c) * sample_size;
      double val = decode_sample(capture->raw_buf + offset, capture->format);
      audio_chunk_get_channel(chunk, c)[f] = val;
    }
  }

  audio_chunk_set_valid_frames(chunk, frames);
  return true;
}

void bluez_capture_close(bluez_capture_t* capture) {
  if (!capture) return;
  capture->active = false;
  if (capture->pipe_fd != -1) {
    close(capture->pipe_fd);
    capture->pipe_fd = -1;
  }
  if (capture->ctrl_fd != -1) {
    close(capture->ctrl_fd);
    capture->ctrl_fd = -1;
  }
}

bool bluez_capture_get_pending_rate_change(bluez_capture_t* capture,
                                           double* out_rate) {
  (void)capture;
  (void)out_rate;
  return false;
}

bool bluez_capture_pitch_control_supported(bluez_capture_t* capture) {
  (void)capture;
  return false;
}

void bluez_capture_set_pitch(bluez_capture_t* capture, double multiplier) {
  (void)capture;
  (void)multiplier;
}

void bluez_capture_destroy(bluez_capture_t* capture) {
  if (!capture) return;
  bluez_capture_close(capture);
  if (capture->raw_buf) {
    free(capture->raw_buf);
  }
  free(capture);
}

#endif  // ENABLE_BLUEZ
