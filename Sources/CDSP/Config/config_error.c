#include "config_error.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/// Errors raised while parsing or validating a `DSPConfiguration`.
void config_error_init(config_error_t* err) {
  if (!err) return;
  err->type = CONFIG_ERR_NONE;
  err->message[0] = '\0';
}

void config_error_set(config_error_t* err, config_error_type_t type,
                      const char* fmt, ...) {
  if (!err) return;
  err->type = type;
  if (fmt) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(err->message, sizeof(err->message), fmt, args);
    va_end(args);
  } else {
    err->message[0] = '\0';
  }
}

void config_error_description(const config_error_t* err, char* out_buf,
                              size_t buf_len) {
  if (!err || !out_buf || buf_len == 0) return;
  switch (err->type) {
    case CONFIG_ERR_PARSE:
      snprintf(out_buf, buf_len, "Parse error: %s", err->message);
      break;
    case CONFIG_ERR_VALIDATION:
      snprintf(out_buf, buf_len, "Validation error: %s", err->message);
      break;
    case CONFIG_ERR_INVALID_FILTER:
      snprintf(out_buf, buf_len, "Invalid filter: %s", err->message);
      break;
    case CONFIG_ERR_INVALID_MIXER:
      snprintf(out_buf, buf_len, "Invalid mixer: %s", err->message);
      break;
    case CONFIG_ERR_INVALID_PIPELINE:
      snprintf(out_buf, buf_len, "Invalid pipeline: %s", err->message);
      break;
    case CONFIG_ERR_NONE:
    default:
      out_buf[0] = '\0';
      break;
  }
}
