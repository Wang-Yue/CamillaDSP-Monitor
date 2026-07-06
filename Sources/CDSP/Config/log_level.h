#ifndef CLIB_CONFIG_LOG_LEVEL_H
#define CLIB_CONFIG_LOG_LEVEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  LOG_LEVEL_OFF = 0,
  LOG_LEVEL_ERROR = 1,
  LOG_LEVEL_WARN = 2,
  LOG_LEVEL_INFO = 3,
  LOG_LEVEL_DEBUG = 4,
  LOG_LEVEL_TRACE = 5
} log_level_t;

/// Compact byte encoding for `Atomic<UInt8>` storage in
/// `MutableLogLevel`. The exact mapping is internal.
uint8_t log_level_to_raw_byte(log_level_t level);
log_level_t log_level_from_raw_byte(uint8_t raw_byte);
const char* log_level_to_string(log_level_t level);
log_level_t log_level_from_string(const char* str);

#ifdef __cplusplus
}
#endif

#endif  // CLIB_CONFIG_LOG_LEVEL_H
