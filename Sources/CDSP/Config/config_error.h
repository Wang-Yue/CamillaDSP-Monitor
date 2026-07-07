#ifndef CLIB_CONFIG_CONFIG_ERROR_H
#define CLIB_CONFIG_CONFIG_ERROR_H

#include <stdbool.h>
#include <stddef.h>

/// Errors raised while parsing or validating a `DSPConfiguration`.
typedef enum {
  CONFIG_ERR_NONE = 0,
  CONFIG_ERR_PARSE,
  CONFIG_ERR_VALIDATION,
  CONFIG_ERR_INVALID_FILTER,
  CONFIG_ERR_INVALID_MIXER,
  CONFIG_ERR_INVALID_PIPELINE
} config_error_type_t;

typedef struct {
  config_error_type_t type;
  char message[512];
} config_error_t;

void config_error_init(config_error_t* err);
void config_error_set(config_error_t* err, config_error_type_t type,
                      const char* fmt, ...);
void config_error_description(const config_error_t* err, char* out_buf,
                              size_t buf_len);

#endif  // CLIB_CONFIG_CONFIG_ERROR_H
