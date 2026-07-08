/**
 * @file log_level.h
 * @brief Logging levels and conversion functions.
 */

#ifndef CLIB_CONFIG_LOG_LEVEL_H
#define CLIB_CONFIG_LOG_LEVEL_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Logging levels.
 */
typedef enum {
  LOG_LEVEL_OFF = 0,   /**< Logging is disabled. */
  LOG_LEVEL_ERROR = 1, /**< Error messages. */
  LOG_LEVEL_WARN = 2,  /**< Warning messages. */
  LOG_LEVEL_INFO = 3,  /**< Informational messages. */
  LOG_LEVEL_DEBUG = 4, /**< Debug messages. */
  LOG_LEVEL_TRACE = 5  /**< Detailed trace messages. */
} log_level_t;

/**
 * @brief Compact byte encoding for `Atomic<UInt8>` storage in
 * `MutableLogLevel`.
 *
 * The exact mapping is internal.
 *
 * @param level The log level to encode.
 * @return The encoded raw byte.
 */
uint8_t log_level_to_raw_byte(log_level_t level);

/**
 * @brief Decodes a log level from a raw byte.
 *
 * @param raw_byte The raw byte to decode.
 * @return The decoded log level.
 */
log_level_t log_level_from_raw_byte(uint8_t raw_byte);

/**
 * @brief Converts a log level to its string representation.
 *
 * @param level The log level to convert.
 * @return A constant string representing the log level.
 */
const char* log_level_to_string(log_level_t level);

/**
 * @brief Parses a log level from its string representation.
 *
 * @param str The string representation of the log level.
 * @return The parsed log level.
 */
log_level_t log_level_from_string(const char* str);

#endif  // CLIB_CONFIG_LOG_LEVEL_H
