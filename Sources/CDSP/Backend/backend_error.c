// Audio backend error definitions.

#include "backend_error.h"

#include <stdio.h>
#include <string.h>

/// Initialize a backend error structure with error type and message.
void backend_error_init(backend_error_t* err, backend_error_type_t type,
                        const char* message) {
  if (!err) return;
  err->type = type;
  if (message) {
    strncpy(err->message, message, sizeof(err->message) - 1);
    err->message[sizeof(err->message) - 1] = '\0';
  } else {
    err->message[0] = '\0';
  }
}

/// Get the string description of a backend error.
const char* backend_error_description(const backend_error_t* err, char* out_buf,
                                      size_t buf_len) {
  if (!err || !out_buf || buf_len == 0) return "";
  switch (err->type) {
    case BACKEND_ERROR_DEVICE_NOT_FOUND:
      snprintf(out_buf, buf_len, "Device not found: %s", err->message);
      break;
    case BACKEND_ERROR_INITIALIZATION_FAILED:
      snprintf(out_buf, buf_len, "Initialization failed: %s", err->message);
      break;
    case BACKEND_ERROR_READ_ERROR:
      snprintf(out_buf, buf_len, "Read error: %s", err->message);
      break;
    case BACKEND_ERROR_WRITE_ERROR:
      snprintf(out_buf, buf_len, "Write error: %s", err->message);
      break;
    default:
      out_buf[0] = '\0';
      break;
  }
  return out_buf;
}

void device_error_init(device_error_t* err, device_error_type_t type,
                       const char* message) {
  if (!err) return;
  err->type = type;
  err->is_error = true;
  if (message) {
    strncpy(err->message, message, sizeof(err->message) - 1);
    err->message[sizeof(err->message) - 1] = '\0';
  } else {
    err->message[0] = '\0';
  }
}

void device_error_clear(device_error_t* err) {
  if (!err) return;
  err->is_error = false;
  err->message[0] = '\0';
}
