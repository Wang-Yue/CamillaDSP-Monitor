#include "log_level.h"

#include <string.h>
#include <strings.h>

/// Compact byte encoding for `Atomic<UInt8>` storage in
/// `MutableLogLevel`. The exact mapping is internal.
uint8_t log_level_to_raw_byte(log_level_t level) {
  switch (level) {
    case LOG_LEVEL_OFF:
      return 0;
    case LOG_LEVEL_ERROR:
      return 1;
    case LOG_LEVEL_WARN:
      return 2;
    case LOG_LEVEL_INFO:
      return 3;
    case LOG_LEVEL_DEBUG:
      return 4;
    case LOG_LEVEL_TRACE:
      return 5;
    default:
      return 3;
  }
}

log_level_t log_level_from_raw_byte(uint8_t raw_byte) {
  switch (raw_byte) {
    case 0:
      return LOG_LEVEL_OFF;
    case 1:
      return LOG_LEVEL_ERROR;
    case 2:
      return LOG_LEVEL_WARN;
    case 4:
      return LOG_LEVEL_DEBUG;
    case 5:
      return LOG_LEVEL_TRACE;
    default:
      return LOG_LEVEL_INFO;
  }
}

const char* log_level_to_string(log_level_t level) {
  switch (level) {
    case LOG_LEVEL_OFF:
      return "Off";
    case LOG_LEVEL_ERROR:
      return "Error";
    case LOG_LEVEL_WARN:
      return "Warn";
    case LOG_LEVEL_INFO:
      return "Info";
    case LOG_LEVEL_DEBUG:
      return "Debug";
    case LOG_LEVEL_TRACE:
      return "Trace";
    default:
      return "Info";
  }
}

log_level_t log_level_from_string(const char* str) {
  if (!str) return LOG_LEVEL_INFO;
  if (strcasecmp(str, "off") == 0)
    return LOG_LEVEL_OFF;
  if (strcasecmp(str, "error") == 0)
    return LOG_LEVEL_ERROR;
  if (strcasecmp(str, "warn") == 0)
    return LOG_LEVEL_WARN;
  if (strcasecmp(str, "info") == 0)
    return LOG_LEVEL_INFO;
  if (strcasecmp(str, "debug") == 0)
    return LOG_LEVEL_DEBUG;
  if (strcasecmp(str, "trace") == 0)
    return LOG_LEVEL_TRACE;
  return LOG_LEVEL_INFO;
}
