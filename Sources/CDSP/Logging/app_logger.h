// Lock-free, allocation-free high performance logger for real-time audio
// threads

#ifndef CLIB_LOGGING_APP_LOGGER_H
#define CLIB_LOGGING_APP_LOGGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Config/log_level.h"

typedef enum {
  LOG_ARG_NONE = 0,
  LOG_ARG_INT = 1,
  LOG_ARG_DOUBLE = 2,
  LOG_ARG_STRING = 3
} log_arg_type_t;

typedef struct {
  log_arg_type_t type;
  union {
    int64_t i;
    double d;
    const char* s;
  } val;
} log_argument_t;

static inline log_argument_t log_arg_none(void) {
  log_argument_t a = {LOG_ARG_NONE, {0}};
  return a;
}
static inline log_argument_t log_arg_int(int64_t i) {
  log_argument_t a;
  a.type = LOG_ARG_INT;
  a.val.i = i;
  return a;
}
static inline log_argument_t log_arg_double(double d) {
  log_argument_t a;
  a.type = LOG_ARG_DOUBLE;
  a.val.d = d;
  return a;
}
static inline log_argument_t log_arg_string(const char* s) {
  log_argument_t a;
  a.type = LOG_ARG_STRING;
  a.val.s = s;
  return a;
}

typedef struct {
  log_level_t level;
  const char* label;
  const char* message;
  log_argument_t arg1;
  log_argument_t arg2;
  log_argument_t arg3;
  log_argument_t arg4;
} log_record_t;

typedef struct app_logger_s app_logger_t;

app_logger_t* app_logger_get_shared(void);

/// Process-wide log-level gate. Stored as an atomic uint8_t so the
/// real-time audio path can read it without locks.
log_level_t app_logger_get_level(void);
void app_logger_set_level(log_level_t level);

void app_logger_log(app_logger_t* logger, log_level_t level, const char* label,
                    const char* message, log_argument_t arg1,
                    log_argument_t arg2, log_argument_t arg3,
                    log_argument_t arg4);

void app_logger_flush_and_stop(app_logger_t* logger);

// Convenience logger helper struct/functions
typedef struct {
  const char* label;
} logger_t;

static inline logger_t logger_create(const char* label) {
  logger_t l = {label};
  return l;
}

static inline void logger_info(const logger_t* logger, const char* msg,
                               log_argument_t a1, log_argument_t a2,
                               log_argument_t a3, log_argument_t a4) {
  app_logger_log(app_logger_get_shared(), LOG_LEVEL_INFO, logger->label, msg,
                 a1, a2, a3, a4);
}

static inline void logger_warn(const logger_t* logger, const char* msg,
                               log_argument_t a1, log_argument_t a2,
                               log_argument_t a3, log_argument_t a4) {
  app_logger_log(app_logger_get_shared(), LOG_LEVEL_WARN, logger->label, msg,
                 a1, a2, a3, a4);
}

static inline void logger_error(const logger_t* logger, const char* msg,
                                log_argument_t a1, log_argument_t a2,
                                log_argument_t a3, log_argument_t a4) {
  app_logger_log(app_logger_get_shared(), LOG_LEVEL_ERROR, logger->label, msg,
                 a1, a2, a3, a4);
}

static inline void logger_debug(const logger_t* logger, const char* msg,
                                log_argument_t a1, log_argument_t a2,
                                log_argument_t a3, log_argument_t a4) {
  app_logger_log(app_logger_get_shared(), LOG_LEVEL_DEBUG, logger->label, msg,
                 a1, a2, a3, a4);
}

#endif  // CLIB_LOGGING_APP_LOGGER_H
