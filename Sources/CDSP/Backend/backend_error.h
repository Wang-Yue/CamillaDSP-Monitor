// Audio backend error definitions.

#ifndef CLIB_BACKEND_BACKEND_ERROR_H
#define CLIB_BACKEND_BACKEND_ERROR_H

#include <stdbool.h>
#include <stddef.h>

/// Errors raised by the audio I/O backends (capture and playback).
typedef enum {
  BACKEND_ERROR_NONE = 0,
  BACKEND_ERROR_DEVICE_NOT_FOUND,
  BACKEND_ERROR_DEVICE_BUSY,
  BACKEND_ERROR_INITIALIZATION_FAILED,
  BACKEND_ERROR_READ_ERROR,
  BACKEND_ERROR_WRITE_ERROR
} backend_error_type_t;

/// Errors raised by the audio I/O backends (capture and playback).
typedef struct {
  backend_error_type_t type;
  char message[256];
} backend_error_t;

/// Initialize a backend error structure with error type and message.
void backend_error_init(backend_error_t* err, backend_error_type_t type,
                        const char* message);

/// Get the string description of a backend error.
const char* backend_error_description(const backend_error_t* err, char* out_buf,
                                      size_t buf_len);

/// Errors returned when probing audio device capabilities (matching Rust's
/// DeviceError enum).
typedef enum {
  DEVICE_ERROR_NOT_FOUND = 0,
  DEVICE_ERROR_BUSY,
  DEVICE_ERROR_OTHER
} device_error_type_t;

typedef struct {
  device_error_type_t type;
  char message[256];
  bool is_error;
} device_error_t;

void device_error_init(device_error_t* err, device_error_type_t type,
                       const char* message);
void device_error_clear(device_error_t* err);

#endif  // CLIB_BACKEND_BACKEND_ERROR_H
