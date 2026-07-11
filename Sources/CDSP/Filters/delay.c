#include "delay.h"

#include "biquad.h"

struct delay_filter {
  char name[64];
  double* queue;
  size_t queue_count;
  size_t read_index;
  biquad_filter_t* biquad;
};

#include <math.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Calculates integer delay and coefficients for fractional delay.
 *
 * If subsample is enabled, this function designs a Thiran allpass filter
 * to approximate the fractional part of the delay.
 *
 * @param delay_samples The total target delay in samples.
 * @param subsample True to enable fractional delay using a Thiran allpass
 * filter.
 * @param[out] out_integer_delay Pointer to store the computed integer delay
 * part.
 * @param[out] out_coeffs Pointer to store the computed biquad coefficients for
 * the fractional part.
 * @param[out] out_has_coeffs Pointer to store a boolean indicating if
 * coefficients were written.
 */
static void build_delay(double delay_samples, bool subsample,
                        int* out_integer_delay,
                        biquad_coefficients_t* out_coeffs,
                        bool* out_has_coeffs) {
  *out_has_coeffs = false;
  if (subsample) {
    // If the delay is very small, we can't design a stable Thiran filter.
    if (delay_samples < 0.1) {
      *out_integer_delay = 0;
      return;
    }
    // For small delays between 0.1 and 1.1, design a 1st order Thiran allpass
    // filter.
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

    // For delays >= 1.1, split the delay into integer and fractional parts.
    double samples = floor(delay_samples);
    double fraction = delay_samples - samples;
    // Shift delay by 1 sample to allow Thiran filter design range to be stable.
    samples -= 1.0;
    fraction += 1.0;
    // Ensure the fraction is in the range [1.1, 2.1) to avoid stability issues
    // near the boundaries.
    if (fraction < 1.1) {
      samples -= 1.0;
      fraction += 1.0;
    }
    // 2nd order Thiran allpass design.
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
    // If subsample is disabled, round to the nearest integer sample.
    *out_integer_delay = (int)round(delay_samples);
  }
}

delay_filter_t* delay_filter_create(const char* name,
                                    const delay_parameters_t* params,
                                    int sample_rate,
                                    config_error_t* err) {
  delay_filter_t* filter = (delay_filter_t*)calloc(1, sizeof(delay_filter_t));
  if (!filter) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate delay filter wrapper");
    return NULL;
  }
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
  if (isnan(delay_samples) || isinf(delay_samples) || delay_samples < 0.0 || delay_samples > 100000000.0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Invalid delay value %f (%d) for filter '%s'",
                     delay, unit, filter->name);
    delay_filter_free(filter);
    return NULL;
  }

  int integer_delay = 0;
  biquad_coefficients_t coeffs;
  bool has_coeffs = false;
  build_delay(delay_samples, subsample, &integer_delay, &coeffs, &has_coeffs);

  if (integer_delay > 0) {
    filter->queue = (double*)calloc(integer_delay, sizeof(double));
    if (!filter->queue) {
      config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate delay line buffer of length %d", integer_delay);
      delay_filter_free(filter);
      return NULL;
    }
    filter->queue_count = integer_delay;
  } else {
    filter->queue = NULL;
    filter->queue_count = 0;
  }
  filter->read_index = 0;
  if (has_coeffs) {
    filter->biquad = biquad_filter_create("delay_biquad", &coeffs, err);
    if (!filter->biquad) {
      delay_filter_free(filter);
      return NULL;
    }
  } else {
    filter->biquad = NULL;
  }
  return filter;
}

void delay_filter_process(delay_filter_t* filter, mutable_waveform_t waveform,
                          size_t count) {
  if (!filter || !waveform || count == 0) return;
  // Apply integer delay using the circular buffer.
  if (filter->queue && filter->queue_count > 0) {
    size_t ri = filter->read_index;
    size_t qc = filter->queue_count;
    double* q = filter->queue;
    for (size_t i = 0; i < count; i++) {
      double delayed = q[ri];
      q[ri] = waveform[i];    // Write current sample to buffer
      waveform[i] = delayed;  // Output delayed sample
      ri++;
      if (ri >= qc) ri = 0;
    }
    filter->read_index = ri;
  }
  // Apply fractional delay filter (Thiran allpass) if configured.
  if (filter->biquad) {
    biquad_filter_process(filter->biquad, waveform, count);
  }
}

double delay_filter_process_single(delay_filter_t* filter, double sample) {
  if (!filter) return sample;
  double out = sample;
  // Apply integer delay using the circular buffer.
  if (filter->queue && filter->queue_count > 0) {
    double delayed = filter->queue[filter->read_index];
    filter->queue[filter->read_index] =
        sample;     // Write current sample to buffer
    out = delayed;  // Output delayed sample
    filter->read_index++;
    if (filter->read_index >= filter->queue_count) filter->read_index = 0;
  }
  // Apply fractional delay filter (Thiran allpass) if configured.
  if (filter->biquad) {
    out = biquad_filter_process_single(filter->biquad, out);
  }
  return out;
}

void delay_filter_free(delay_filter_t* filter) {
  if (!filter) return;
  if (filter->queue) free(filter->queue);
  if (filter->biquad) biquad_filter_free(filter->biquad);
  free(filter);
}
