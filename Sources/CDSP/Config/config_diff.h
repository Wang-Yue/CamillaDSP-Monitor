#ifndef CLIB_CONFIG_CONFIG_DIFF_H
#define CLIB_CONFIG_CONFIG_DIFF_H

#include "configuration.h"

typedef enum {
  CONFIG_CHANGE_NONE = 0,
  CONFIG_CHANGE_FILTER_PARAMETERS,
  CONFIG_CHANGE_MIXER_PARAMETERS,
  CONFIG_CHANGE_PIPELINE,
  CONFIG_CHANGE_DEVICES
} config_change_type_t;

typedef struct {
  config_change_type_t type;
  char** filters;
  size_t filters_count;
  char** mixers;
  size_t mixers_count;
  char** processors;
  size_t processors_count;
} config_change_t;

config_change_type_t config_diff(const dsp_config_t* current,
                                 const dsp_config_t* new_conf,
                                 config_change_t* out_change);

void config_change_free(config_change_t* change);

#endif  // CLIB_CONFIG_CONFIG_DIFF_H
