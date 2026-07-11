// Audio backend error definitions.

#ifndef CLIB_BACKEND_BACKEND_ERROR_H
#define CLIB_BACKEND_BACKEND_ERROR_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @file backend_error.h
 * @brief Audio backend error definitions.
 *
 * This header defines error types and structures used by the audio backends
 * for capture and playback, as well as device capability probing.
 */

/**
 * @brief Errors raised by the audio I/O backends (capture and playback).
 */
typedef enum {
  BACKEND_ERROR_NONE = 0,              /**< No error. */
  BACKEND_ERROR_DEVICE_NOT_FOUND,      /**< Audio device was not found. */
  BACKEND_ERROR_DEVICE_BUSY,           /**< Audio device is busy. */
  BACKEND_ERROR_INITIALIZATION_FAILED, /**< Backend initialization failed. */
  BACKEND_ERROR_READ_ERROR,            /**< Error reading from device. */
  BACKEND_ERROR_WRITE_ERROR,           /**< Error writing to device. */
  BACKEND_ERROR_READ_EOF,              /**< End of file/stream reached. */
  BACKEND_ERROR_INVALID_CHANNELS       /**< Invalid channel count. */
} backend_error_type_t;

/**
 * @brief Structure representing a backend error.
 */
typedef struct {
  backend_error_type_t type; /**< The type of the error. */
  char message[256];         /**< Detailed error message. */
} backend_error_t;

/**
 * @brief Initialize a backend error structure with error type and message.
 *
 * @param err Pointer to the backend error structure to initialize.
 * @param type The type of error.
 * @param message The detailed error message (will be copied).
 */
void backend_error_init(backend_error_t* err, backend_error_type_t type,
                        const char* message);

/**
 * @brief Get the string description of a backend error.
 *
 * Fills the output buffer with a description of the error.
 *
 * @param err Pointer to the backend error structure.
 * @param out_buf Output buffer to write the description into.
 * @param buf_len Length of the output buffer.
 * @return Pointer to the output buffer containing the description.
 */
const char* backend_error_description(const backend_error_t* err, char* out_buf,
                                      size_t buf_len);

/**
 * @brief Errors returned when probing audio device capabilities.
 *
 * These match Rust's DeviceError enum.
 */
typedef enum {
  DEVICE_ERROR_NOT_FOUND = 0, /**< Device not found. */
  DEVICE_ERROR_BUSY,          /**< Device is busy. */
  DEVICE_ERROR_OTHER          /**< Other error. */
} device_error_type_t;

/**
 * @brief Structure representing a device error.
 */
typedef struct {
  device_error_type_t type; /**< The type of the device error. */
  char message[256];        /**< Detailed error message. */
  bool is_error;            /**< Flag indicating if this represents an error. */
} device_error_t;

/**
 * @brief Initialize a device error structure.
 *
 * @param err Pointer to the device error structure to initialize.
 * @param type The type of device error.
 * @param message The detailed error message.
 */
void device_error_init(device_error_t* err, device_error_type_t type,
                       const char* message);

/**
 * @brief Clear a device error structure (resetting it).
 *
 * @param err Pointer to the device error structure to clear.
 */
void device_error_clear(device_error_t* err);

#endif  // CLIB_BACKEND_BACKEND_ERROR_H
