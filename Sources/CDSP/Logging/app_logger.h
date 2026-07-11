// Lock-free, allocation-free high performance logger for real-time audio
// threads

#ifndef CLIB_LOGGING_APP_LOGGER_H
#define CLIB_LOGGING_APP_LOGGER_H

/**
 * @file app_logger.h
 * @brief Lock-free, allocation-free logger for real-time audio threads.
 *
 * This module provides a logging framework optimized for execution in real-time
 * context, ensuring no memory allocations or blocking locks are encountered.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "Config/log_level.h"

/**
 * @brief Types of arguments that can be passed to log statements.
 */
typedef enum {
  LOG_ARG_NONE = 0,   /**< No argument */
  LOG_ARG_INT = 1,    /**< Integer argument (int64_t) */
  LOG_ARG_DOUBLE = 2, /**< Double argument (double) */
  LOG_ARG_STRING = 3  /**< String argument (const char*) */
} log_arg_type_t;

/**
 * @brief A container for a single log statement argument.
 */
typedef struct {
  log_arg_type_t type; /**< The type of the argument. */
  union {
    int64_t i;     /**< Integer value. */
    double d;      /**< Double value. */
    char s[8192];  /**< String value copy. */
  } val;           /**< The union containing the argument value. */
} log_argument_t;

/**
 * @brief Creates an empty log argument.
 * @return An empty log_argument_t.
 */
static inline log_argument_t log_arg_none(void) {
  log_argument_t a = {LOG_ARG_NONE, {0}};
  return a;
}

/**
 * @brief Creates an integer log argument.
 * @param i The integer value.
 * @return A log_argument_t containing the integer.
 */
static inline log_argument_t log_arg_int(int64_t i) {
  log_argument_t a;
  a.type = LOG_ARG_INT;
  a.val.i = i;
  return a;
}

/**
 * @brief Creates a double precision floating point log argument.
 * @param d The double value.
 * @return A log_argument_t containing the double.
 */
static inline log_argument_t log_arg_double(double d) {
  log_argument_t a;
  a.type = LOG_ARG_DOUBLE;
  a.val.d = d;
  return a;
}

/**
 * @brief Creates a string log argument.
 * @param s The string pointer.
 * @return A log_argument_t containing the string.
 */
static inline log_argument_t log_arg_string(const char* s) {
  log_argument_t a;
  a.type = LOG_ARG_STRING;
  if (s) {
    size_t len = strlen(s);
    size_t max_len = sizeof(a.val.s) - 1;
    if (len > max_len) {
      len = max_len;
    }
    memcpy(a.val.s, s, len);
    a.val.s[len] = '\0';
  } else {
    a.val.s[0] = '\0';
  }
  return a;
}

/**
 * @brief Represents a single log record entry.
 */
typedef struct {
  log_level_t level;   /**< Log level of this record. */
  const char* label;   /**< Component label. */
  const char* message; /**< Message format string. */
  log_argument_t arg1; /**< First optional argument. */
  log_argument_t arg2; /**< Second optional argument. */
  log_argument_t arg3; /**< Third optional argument. */
  log_argument_t arg4; /**< Fourth optional argument. */
} log_record_t;

/**
 * @brief Opaque struct representing the logger backend.
 */
typedef struct app_logger_s app_logger_t;

/**
 * @brief Gets the shared singleton instance of the logger.
 * @return Pointer to the shared app_logger_t instance.
 */
app_logger_t* app_logger_get_shared(void);

/**
 * @brief Gets the process-wide log level.
 *
 * Stored as an atomic uint8_t so the real-time audio path can read it without
 * locks.
 *
 * @return The current log_level_t.
 */
log_level_t app_logger_get_level(void);

/**
 * @brief Sets the process-wide log level.
 * @param level The new log_level_t.
 */
void app_logger_set_level(log_level_t level);

/**
 * @brief Logs a message using the specified logger.
 *
 * @param logger Pointer to the logger instance.
 * @param level Log severity level.
 * @param label Component label.
 * @param message Message format string.
 * @param arg1 First optional argument.
 * @param arg2 Second optional argument.
 * @param arg3 Third optional argument.
 * @param arg4 Fourth optional argument.
 */
void app_logger_log(app_logger_t* logger, log_level_t level, const char* label,
                    const char* message, log_argument_t arg1,
                    log_argument_t arg2, log_argument_t arg3,
                    log_argument_t arg4);

/**
 * @brief Flushes any pending log records and stops the logger thread/backend.
 * @param logger Pointer to the logger instance.
 */
void app_logger_flush_and_stop(app_logger_t* logger);

/**
 * @brief Logger handle used for components.
 */
typedef struct {
  const char*
      label; /**< Label prepended to all messages logged using this handle. */
} logger_t;

/**
 * @brief Creates a logger handle with a specific label.
 * @param label The label string.
 * @return A new logger_t handle.
 */
static inline logger_t logger_create(const char* label) {
  logger_t l = {label};
  return l;
}

/**
 * @brief Logs an informational message.
 * @param logger Pointer to the logger handle.
 * @param msg Message format string.
 * @param a1 First optional argument.
 * @param a2 Second optional argument.
 * @param a3 Third optional argument.
 * @param a4 Fourth optional argument.
 */
static inline void logger_info(const logger_t* logger, const char* msg,
                               log_argument_t a1, log_argument_t a2,
                               log_argument_t a3, log_argument_t a4) {
  app_logger_log(app_logger_get_shared(), LOG_LEVEL_INFO, logger->label, msg,
                 a1, a2, a3, a4);
}

/**
 * @brief Logs a warning message.
 * @param logger Pointer to the logger handle.
 * @param msg Message format string.
 * @param a1 First optional argument.
 * @param a2 Second optional argument.
 * @param a3 Third optional argument.
 * @param a4 Fourth optional argument.
 */
static inline void logger_warn(const logger_t* logger, const char* msg,
                               log_argument_t a1, log_argument_t a2,
                               log_argument_t a3, log_argument_t a4) {
  app_logger_log(app_logger_get_shared(), LOG_LEVEL_WARN, logger->label, msg,
                 a1, a2, a3, a4);
}

/**
 * @brief Logs an error message.
 * @param logger Pointer to the logger handle.
 * @param msg Message format string.
 * @param a1 First optional argument.
 * @param a2 Second optional argument.
 * @param a3 Third optional argument.
 * @param a4 Fourth optional argument.
 */
static inline void logger_error(const logger_t* logger, const char* msg,
                                log_argument_t a1, log_argument_t a2,
                                log_argument_t a3, log_argument_t a4) {
  app_logger_log(app_logger_get_shared(), LOG_LEVEL_ERROR, logger->label, msg,
                 a1, a2, a3, a4);
}

/**
 * @brief Logs a debugging message.
 * @param logger Pointer to the logger handle.
 * @param msg Message format string.
 * @param a1 First optional argument.
 * @param a2 Second optional argument.
 * @param a3 Third optional argument.
 * @param a4 Fourth optional argument.
 */
static inline void logger_debug(const logger_t* logger, const char* msg,
                                log_argument_t a1, log_argument_t a2,
                                log_argument_t a3, log_argument_t a4) {
  app_logger_log(app_logger_get_shared(), LOG_LEVEL_DEBUG, logger->label, msg,
                 a1, a2, a3, a4);
}

#endif  // CLIB_LOGGING_APP_LOGGER_H
