#ifndef CLIB_FILTERS_DIFFEQ_H
#define CLIB_FILTERS_DIFFEQ_H

#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/filter_config_types.h"

// Difference equation filter (Direct Form I / II IIR/FIR filter
// implementation). Normalize by a[0]
typedef struct {
  char name[64];
  double* x;
  double* y;
  double* a;
  double* b;
  size_t a_count;
  size_t b_count;
  size_t idx_x;
  size_t idx_y;
} diffeq_filter_t;

diffeq_filter_t* diffeq_filter_create(const char* name,
                                      const diff_eq_parameters_t* params);
void diffeq_filter_process(diffeq_filter_t* filter, mutable_waveform_t waveform,
                           size_t count);
void diffeq_filter_update_parameters(diffeq_filter_t* filter,
                                     const filter_config_t* config,
                                     int sample_rate);
void diffeq_filter_free(diffeq_filter_t* filter);

#endif  // CLIB_FILTERS_DIFFEQ_H
