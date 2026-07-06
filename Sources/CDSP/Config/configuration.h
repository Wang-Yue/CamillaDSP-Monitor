#ifndef CLIB_CONFIG_CONFIGURATION_H
#define CLIB_CONFIG_CONFIGURATION_H

// Top-level configuration data structures and validation logic. The JSON loader
// lives in `config_loader.c`.
//
// This file owns:
//   1. Top-level configuration models (dsp_config_t and pipeline_step_t).
//   2. Cross-component validation logic, including schema checks and the
//      pipeline walk that tracks channel layouts.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config_error.h"
#include "engine_config_types.h"
#include "filter_config_types.h"
#include "mixer_config_types.h"
#include "processor_config_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  PIPELINE_STEP_TYPE_FILTER = 0,
  PIPELINE_STEP_TYPE_MIXER,
  PIPELINE_STEP_TYPE_PROCESSOR
} pipeline_step_type_t;

/// One step in the user-defined processing pipeline. Either a named
/// filter chain applied to one or more channels, or a mixer that
/// changes the channel layout.
typedef struct {
  pipeline_step_type_t type;
  int channel;
  bool has_channel;
  int* channels;
  size_t channels_count;
  char name[128];
  bool has_name;
  char** names;
  size_t names_count;
  bool bypassed;
} pipeline_step_t;

typedef struct {
  char name[128];
  filter_config_t filter;
} named_filter_config_t;

typedef struct {
  char name[128];
  mixer_config_t mixer;
} named_mixer_config_t;

typedef struct {
  char name[128];
  processor_config_t processor;
} named_processor_config_t;

/// Top-level configuration consumed by the DSP engine.
typedef struct {
  devices_config_t devices;
  named_filter_config_t* filters;
  size_t filters_count;
  named_mixer_config_t* mixers;
  size_t mixers_count;
  named_processor_config_t* processors;
  size_t processors_count;
  pipeline_step_t* pipeline;
  size_t pipeline_count;
} dsp_config_t;

int dsp_config_validate(const dsp_config_t* config, config_error_t* err);
int dsp_config_parse_json(const char* json, dsp_config_t** out_config,
                          config_error_t* err);
void dsp_config_free(dsp_config_t* config);

filter_config_t* dsp_config_get_filter(const dsp_config_t* config,
                                       const char* name);
mixer_config_t* dsp_config_get_mixer(const dsp_config_t* config,
                                     const char* name);
processor_config_t* dsp_config_get_processor(const dsp_config_t* config,
                                             const char* name);

#ifdef __cplusplus
}
#endif

#endif  // CLIB_CONFIG_CONFIGURATION_H
