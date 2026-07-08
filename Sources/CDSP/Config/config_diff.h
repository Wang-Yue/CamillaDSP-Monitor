#ifndef CLIB_CONFIG_CONFIG_DIFF_H
#define CLIB_CONFIG_CONFIG_DIFF_H

#include <stddef.h>
#include "configuration.h"

typedef enum {
  CONFIG_CHANGE_NONE = 0,
  CONFIG_CHANGE_FILTER_PARAMETERS,
  CONFIG_CHANGE_MIXER_PARAMETERS,
  CONFIG_CHANGE_PIPELINE,
  CONFIG_CHANGE_DEVICES
} config_change_type_t;

// Opaque struct type definition
typedef struct config_change config_change_t;

config_change_t* config_change_create(void);
void config_change_free(config_change_t* change);

config_change_type_t config_diff(const dsp_config_t* current,
                                 const dsp_config_t* new_conf,
                                 config_change_t* change);

// Accessors that transfer ownership of the name arrays to the caller
char** config_change_take_filters(config_change_t* change, size_t* out_count);
char** config_change_take_mixers(config_change_t* change, size_t* out_count);
char** config_change_take_processors(config_change_t* change, size_t* out_count);

#endif  // CLIB_CONFIG_CONFIG_DIFF_H
