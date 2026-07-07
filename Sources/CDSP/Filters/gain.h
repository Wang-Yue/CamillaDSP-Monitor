#ifndef CLIB_FILTERS_GAIN_H
#define CLIB_FILTERS_GAIN_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/filter_config_types.h"

typedef struct {
  char name[64];
  double linear_gain;
  bool muted;
} gain_filter_t;

gain_filter_t* gain_filter_create(const char* name,
                                  const gain_parameters_t* params);
void gain_filter_process(gain_filter_t* filter, mutable_waveform_t waveform,
                         size_t count);
double gain_filter_process_single(gain_filter_t* filter, double sample);
void gain_filter_update_parameters(gain_filter_t* filter,
                                   const filter_config_t* config,
                                   int sample_rate);
void gain_filter_free(gain_filter_t* filter);

#endif  // CLIB_FILTERS_GAIN_H
