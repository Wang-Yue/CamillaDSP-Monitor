#include "delay.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static double compute_delay_samples(double delay, delay_unit_t unit,
                                    int sample_rate) {
  switch (unit) {
    case DELAY_UNIT_MS:
      return delay / 1000.0 * (double)sample_rate;
    case DELAY_UNIT_US:
      return delay / 1000000.0 * (double)sample_rate;
    case DELAY_UNIT_SAMPLES:
      return delay;
    case DELAY_UNIT_MM:
      return delay / 1000.0 * (double)sample_rate / 343.0;
    default:
      return delay;
  }
}

/// Builds the subsample biquad allpass and returns (integerDelaySamples,
/// optionalBiquad).
static void build_delay(double delay_samples, bool subsample,
                        int* out_integer_delay,
                        biquad_coefficients_t* out_coeffs,
                        bool* out_has_coeffs) {
  *out_has_coeffs = false;
  if (subsample) {
    if (delay_samples < 0.1) {
      *out_integer_delay = 0;
      return;
    }
    if (delay_samples < 1.1) {
      double coeff = (1.0 - delay_samples) / (1.0 + delay_samples);
      // 1st order Thiran allpass: coeffs a1 = coeff, b0 = coeff, b1 = 1.0, b2 =
      // 0.0, a2 = 0.0
      out_coeffs->b0 = coeff;
      out_coeffs->b1 = 1.0;
      out_coeffs->b2 = 0.0;
      out_coeffs->a1 = coeff;
      out_coeffs->a2 = 0.0;
      *out_integer_delay = 0;
      *out_has_coeffs = true;
      return;
    }
    double samples = floor(delay_samples);
    double fraction = delay_samples - samples;
    samples -= 1.0;
    fraction += 1.0;
    if (fraction < 1.1) {
      samples -= 1.0;
      fraction += 1.0;
    }
    // 2nd order Thiran allpass
    double coeff1 = 2.0 * (2.0 - fraction) / (1.0 + fraction);
    double coeff2 = ((2.0 - fraction) / (2.0 + fraction)) *
                    ((1.0 - fraction) / (1.0 + fraction));
    out_coeffs->b0 = coeff2;
    out_coeffs->b1 = coeff1;
    out_coeffs->b2 = 1.0;
    out_coeffs->a1 = coeff1;
    out_coeffs->a2 = coeff2;
    *out_integer_delay = (int)samples;
    *out_has_coeffs = true;
  } else {
    *out_integer_delay = (int)round(delay_samples);
  }
}

delay_filter_t* delay_filter_create(const char* name,
                                    const delay_parameters_t* params,
                                    int sample_rate) {
  delay_filter_t* filter = (delay_filter_t*)malloc(sizeof(delay_filter_t));
  if (!filter) return NULL;
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "delay");
  }

  double delay = params ? params->delay : 0.0;
  delay_unit_t unit = params ? params->unit : DELAY_UNIT_MS;
  bool subsample = params ? params->subsample : false;

  double delay_samples = compute_delay_samples(delay, unit, sample_rate);
  int integer_delay = 0;
  biquad_coefficients_t coeffs;
  bool has_coeffs = false;
  build_delay(delay_samples, subsample, &integer_delay, &coeffs, &has_coeffs);

  if (integer_delay > 0) {
    filter->queue = (double*)calloc(integer_delay, sizeof(double));
    filter->queue_count = integer_delay;
  } else {
    filter->queue = NULL;
    filter->queue_count = 0;
  }
  filter->read_index = 0;
  if (has_coeffs) {
    filter->biquad = biquad_filter_create("delay_biquad", &coeffs);
  } else {
    filter->biquad = NULL;
  }
  return filter;
}

void delay_filter_process(delay_filter_t* filter, mutable_waveform_t waveform,
                          size_t count) {
  if (!filter || !waveform || count == 0) return;
  if (filter->queue && filter->queue_count > 0) {
    size_t ri = filter->read_index;
    size_t qc = filter->queue_count;
    double* q = filter->queue;
    for (size_t i = 0; i < count; i++) {
      double delayed = q[ri];
      q[ri] = waveform[i];
      waveform[i] = delayed;
      ri++;
      if (ri >= qc) ri = 0;
    }
    filter->read_index = ri;
  }
  if (filter->biquad) {
    biquad_filter_process(filter->biquad, waveform, count);
  }
}

double delay_filter_process_single(delay_filter_t* filter, double sample) {
  if (!filter) return sample;
  double out = sample;
  if (filter->queue && filter->queue_count > 0) {
    double delayed = filter->queue[filter->read_index];
    filter->queue[filter->read_index] = sample;
    out = delayed;
    filter->read_index++;
    if (filter->read_index >= filter->queue_count) filter->read_index = 0;
  }
  if (filter->biquad) {
    out = biquad_filter_process_single(filter->biquad, out);
  }
  return out;
}

void delay_filter_update_parameters(delay_filter_t* filter,
                                    const filter_config_t* config,
                                    int sample_rate) {
  if (!filter || !config) return;
  if (config->type != FILTER_TYPE_DELAY) return;
  const delay_parameters_t* params = &config->parameters.delay;

  double delay_samples =
      compute_delay_samples(params->delay, params->unit, sample_rate);
  int integer_delay = 0;
  biquad_coefficients_t coeffs;
  bool has_coeffs = false;
  build_delay(delay_samples, params->subsample, &integer_delay, &coeffs,
              &has_coeffs);

  if (filter->queue) {
    free(filter->queue);
  }
  if (integer_delay > 0) {
    filter->queue = (double*)calloc(integer_delay, sizeof(double));
    filter->queue_count = integer_delay;
  } else {
    filter->queue = NULL;
    filter->queue_count = 0;
  }
  filter->read_index = 0;

  if (filter->biquad) {
    biquad_filter_free(filter->biquad);
  }
  if (has_coeffs) {
    filter->biquad = biquad_filter_create("delay_biquad", &coeffs);
  } else {
    filter->biquad = NULL;
  }
}

void delay_filter_free(delay_filter_t* filter) {
  if (!filter) return;
  if (filter->queue) free(filter->queue);
  if (filter->biquad) biquad_filter_free(filter->biquad);
  free(filter);
}
