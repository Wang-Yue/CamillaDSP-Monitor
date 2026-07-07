#ifndef CLIB_FILTERS_LOOKAHEAD_LIMITER_H
#define CLIB_FILTERS_LOOKAHEAD_LIMITER_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/config_error.h"
#include "Config/filter_config_types.h"

typedef struct {
  char name[64];
  double limit;
  int attack_samples;
  double release_coeff;
  // Inlined LookaheadBuffer
  double* lookahead_data;
  size_t lookahead_capacity;
  size_t lookahead_read_index;
  size_t lookahead_write_index;
  double release_gain;
  // Pre-allocated output buffer to avoid heap allocation on the hot path
  double* output_buffer;
  size_t output_buffer_capacity;
} lookahead_limiter_filter_t;

int lookahead_limiter_parameters_validate(
    const lookahead_limiter_parameters_t* params, int sample_rate,
    config_error_t* err);
lookahead_limiter_filter_t* lookahead_limiter_filter_create(
    const char* name, const lookahead_limiter_parameters_t* params,
    int sample_rate, size_t chunk_size);
void lookahead_limiter_filter_process(lookahead_limiter_filter_t* filter,
                                      mutable_waveform_t waveform,
                                      size_t count);
void lookahead_limiter_filter_update_parameters(
    lookahead_limiter_filter_t* filter, const filter_config_t* config,
    int sample_rate);
void lookahead_limiter_filter_free(lookahead_limiter_filter_t* filter);

#endif  // CLIB_FILTERS_LOOKAHEAD_LIMITER_H
