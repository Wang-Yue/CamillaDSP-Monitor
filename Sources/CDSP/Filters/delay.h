#ifndef CLIB_FILTERS_DELAY_H
#define CLIB_FILTERS_DELAY_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/filter_config_types.h"
#include "biquad.h"

// Delay filter with optional subsample interpolation using Thiran allpass
// biquads.
typedef struct {
  char name[64];
  double* queue;
  size_t queue_count;
  size_t read_index;
  biquad_filter_t* biquad;
} delay_filter_t;

/// Builds the subsample biquad allpass and returns (integerDelaySamples,
/// optionalBiquad). 1st order Thiran allpass: coeffs a1 = coeff, b0 = coeff, b1
/// = 1.0, b2 = 0.0, a2 = 0.0 2nd order Thiran allpass
delay_filter_t* delay_filter_create(const char* name,
                                    const delay_parameters_t* params,
                                    int sample_rate);
void delay_filter_process(delay_filter_t* filter, mutable_waveform_t waveform,
                          size_t count);
double delay_filter_process_single(delay_filter_t* filter, double sample);
void delay_filter_update_parameters(delay_filter_t* filter,
                                    const filter_config_t* config,
                                    int sample_rate);
void delay_filter_free(delay_filter_t* filter);

#endif  // CLIB_FILTERS_DELAY_H
