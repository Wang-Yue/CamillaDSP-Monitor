#ifndef CLIB_FILTERS_DITHER_H
#define CLIB_FILTERS_DITHER_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/filter_config_types.h"

// MARK: - Ditherers

// MARK: - NoiseShaper
typedef struct {
  double* filter;
  double* buffer;
  size_t filter_count;
  size_t write_index;
} noise_shaper_t;

noise_shaper_t* noise_shaper_create(const double* filter_coeffs, size_t count);
double noise_shaper_process(noise_shaper_t* shaper, double scaled,
                            double dither);
void noise_shaper_free(noise_shaper_t* shaper);

// MARK: - Noise Shaper Factory
noise_shaper_t* noise_shaper_create_for_type(dither_type_t type);

// MARK: - DitherFilter
typedef struct {
  char name[64];
  dither_type_t type;
  double scalefact;
  double amplitude;
  noise_shaper_t* shaper;
  double previous_sample;
} dither_filter_t;

dither_filter_t* dither_filter_create(const char* name,
                                      const dither_parameters_t* params);
void dither_filter_process(dither_filter_t* filter, mutable_waveform_t waveform,
                           size_t count);
void dither_filter_update_parameters(dither_filter_t* filter,
                                     const filter_config_t* config,
                                     int sample_rate);
void dither_filter_free(dither_filter_t* filter);

#endif  // CLIB_FILTERS_DITHER_H
