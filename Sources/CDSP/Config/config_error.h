/**
 * @file config_error.h
 * @brief Error handling types and functions for configuration parsing and
 * validation.
 */

#ifndef CLIB_CONFIG_CONFIG_ERROR_H
#define CLIB_CONFIG_CONFIG_ERROR_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Errors raised while parsing or validating a `DSPConfiguration`.
 */
typedef enum {
  CONFIG_ERR_NONE = 0,   /**< No error. */
  CONFIG_ERR_PARSE,      /**< Error parsing the configuration (e.g., JSON syntax
                            error). */
  CONFIG_ERR_VALIDATION, /**< General validation error. */
  CONFIG_ERR_INVALID_FILTER,  /**< Invalid filter configuration. */
  CONFIG_ERR_INVALID_MIXER,   /**< Invalid mixer configuration. */
  CONFIG_ERR_INVALID_PIPELINE /**< Invalid pipeline configuration. */
} config_error_type_t;

/**
 * @brief Structure representing a configuration error.
 */
typedef struct {
  config_error_type_t type; /**< The type of the error. */
  char message[512];        /**< A detailed error message. */
} config_error_t;

/**
 * @brief Initializes a config_error_t struct.
 *
 * Sets the error type to CONFIG_ERR_NONE and clears the message.
 *
 * @param err Pointer to the config_error_t struct to initialize.
 */
void config_error_init(config_error_t* err);

/**
 * @brief Sets a configuration error.
 *
 * Formats the error message using printf-style formatting.
 *
 * @param err Pointer to the config_error_t struct to set.
 * @param type The type of the error.
 * @param fmt Format string for the error message.
 * @param ... Additional arguments for the format string.
 */
void config_error_set(config_error_t* err, config_error_type_t type,
                      const char* fmt, ...);

/**
 * @brief Gets a description of the configuration error.
 *
 * Fills the output buffer with a string representation of the error.
 *
 * @param err Pointer to the config_error_t struct.
 * @param out_buf Pointer to the output buffer where the description will be
 * written.
 * @param buf_len Length of the output buffer.
 */
void config_error_description(const config_error_t* err, char* out_buf,
                              size_t buf_len);

#endif  // CLIB_CONFIG_CONFIG_ERROR_H
