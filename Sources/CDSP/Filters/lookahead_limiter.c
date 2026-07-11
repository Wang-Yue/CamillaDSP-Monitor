#include "lookahead_limiter.h"

struct lookahead_limiter_filter {
  char name[64];
  double limit;
  int attack_samples;
  double release_coeff;
  double* lookahead_data;
  size_t lookahead_capacity;
  size_t lookahead_read_index;
  size_t lookahead_write_index;
  double release_gain;
  double* output_buffer;
  size_t output_buffer_capacity;
};

#include <math.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Parses parameters and computes internal filter settings.
 *
 * @param params User-provided filter parameters.
 * @param sample_rate The sample rate.
 * @param out_limit Output pointer for the computed linear gain limit.
 * @param out_attack_samples Output pointer for attack time in samples.
 * @param out_release_coeff Output pointer for exponential release coefficient.
 */
static void configure(const lookahead_limiter_parameters_t* params,
                      int sample_rate, double* out_limit,
                      int* out_attack_samples, double* out_release_coeff) {
  double limit_db = params ? params->limit : 0.0;
  *out_limit = double_from_db(limit_db);
  delay_unit_t unit = params ? params->unit : DELAY_UNIT_MS;
  double attack = params ? params->attack : 0.0;
  double release = params ? params->release : 0.0;
  *out_attack_samples =
      (int)round(compute_delay_samples(attack, unit, sample_rate));
  double release_samples = compute_delay_samples(release, unit, sample_rate);
  if (release_samples > 0.0) {
    *out_release_coeff = exp(-1.0 / release_samples);
  } else {
    *out_release_coeff = 0.0;
  }
}

/**
 * @brief Pushes a sample into the lookahead circular buffer, overwriting the
 * oldest sample.
 *
 * @param filter The limiter filter instance.
 * @param sample The input sample value.
 */
static inline void push_overwrite(lookahead_limiter_filter_t* filter,
                                  double sample) {
  filter->lookahead_data[filter->lookahead_write_index] = sample;
  filter->lookahead_write_index =
      (filter->lookahead_write_index + 1) % filter->lookahead_capacity;
  filter->lookahead_read_index =
      (filter->lookahead_read_index + 1) % filter->lookahead_capacity;
}

/**
 * @brief Retrieves a sample from the circular lookahead buffer at an offset
 * from read index.
 *
 * @param filter The limiter filter instance.
 * @param idx Offset relative to the current read index.
 * @return The sample value.
 */
static inline double get_occupied(lookahead_limiter_filter_t* filter,
                                  size_t idx) {
  size_t real_idx =
      (filter->lookahead_read_index + idx) % filter->lookahead_capacity;
  return filter->lookahead_data[real_idx];
}

lookahead_limiter_filter_t* lookahead_limiter_filter_create(
    const char* name, const lookahead_limiter_parameters_t* params,
    int sample_rate, size_t chunk_size) {
  lookahead_limiter_filter_t* filter =
      (lookahead_limiter_filter_t*)calloc(1, sizeof(lookahead_limiter_filter_t));
  if (!filter) return NULL;
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "lookahead_limiter");
  }

  double limit;
  int attack_samples;
  double release_coeff;
  configure(params, sample_rate, &limit, &attack_samples, &release_coeff);

  if (limit <= 0.0 || !isfinite(limit)) {
    lookahead_limiter_filter_free(filter);
    return NULL;
  }
  filter->limit = limit;
  filter->attack_samples = attack_samples;
  filter->release_coeff = release_coeff;
  filter->release_gain = 1.0;

  // Inlined LookaheadBuffer
  size_t lookahead_len =
      (size_t)sample_rate > chunk_size ? (size_t)sample_rate : chunk_size;
  if (lookahead_len < 1024) lookahead_len = 1024;
  filter->lookahead_capacity = lookahead_len;
  filter->lookahead_data = (double*)calloc(lookahead_len, sizeof(double));
  if (!filter->lookahead_data) {
    lookahead_limiter_filter_free(filter);
    return NULL;
  }
  filter->lookahead_read_index = 0;
  filter->lookahead_write_index = 0;

  // Pre-allocated output buffer to avoid heap allocation on the hot path
  size_t out_cap = chunk_size > 8192 ? chunk_size : 8192;
  filter->output_buffer_capacity = out_cap;
  filter->output_buffer = (double*)calloc(out_cap, sizeof(double));
  if (!filter->output_buffer) {
    lookahead_limiter_filter_free(filter);
    return NULL;
  }

  return filter;
}

/**
 * @brief Processes a slice of the waveform.
 *
 * This function implements a two-pass lookahead limiter algorithm:
 * 1. A backward pass that scans future samples (including the lookahead delay
 * buffer) and calculates the required gain reduction to prevent clipping,
 * applying a linear ramp-down (attack) leading up to any peak.
 * 2. A forward pass that applies an exponential release to the gain reduction
 * envelope.
 * 3. Finally, it applies the computed gain envelope to the delayed input
 * samples.
 *
 * @param filter The limiter filter instance.
 * @param waveform The current block of audio samples to process.
 * @param len The length of the slice (must be <= output_buffer_capacity).
 */
static void process_slice(lookahead_limiter_filter_t* filter,
                          mutable_waveform_t waveform, size_t len) {
  // The oldest sample in the lookahead buffer (delay line) starts at this
  // offset relative to the capacity.
  size_t lookahead_start = filter->lookahead_capacity - filter->attack_samples;
  double peak = 1.0;
  int samples_since_peak = filter->attack_samples + 1;

  // Backward pass: Scan from the future (end of the new waveform block)
  // to the past (oldest samples in the delay line).
  // This calculates the necessary gain reduction to smoothly anticipate peaks.
  for (int i = (int)(filter->attack_samples + len) - 1; i >= 0; i--) {
    double input_sample;
    if (i < filter->attack_samples) {
      // Access samples in the lookahead delay line (older inputs).
      input_sample = get_occupied(filter, lookahead_start + i);
    } else {
      // Access new input samples from the current block.
      input_sample = waveform[i - filter->attack_samples];
    }

    double amplitude = fabs(input_sample);
    // If the sample exceeds the limit, compute the required attenuation factor.
    double gain = amplitude > filter->limit ? (filter->limit / amplitude) : 1.0;

    // Smoothly ramp down the gain leading up to a peak (going backward, this
    // looks like ramping up to 1.0 from the peak).
    double ramp_gain = 1.0;
    if (samples_since_peak <= filter->attack_samples) {
      double ramp =
          (double)(filter->attack_samples - samples_since_peak) /
          (double)(filter->attack_samples > 1 ? filter->attack_samples : 1);
      ramp_gain = 1.0 - (ramp * (1.0 - peak));
      samples_since_peak++;
    }

    // If the current sample requires more attenuation than the ramp,
    // establish a new peak.
    if (gain < ramp_gain) {
      peak = gain;
      samples_since_peak = 1;
    } else {
      gain = ramp_gain;
    }

    // We only output gain values for the current block (length len).
    if (i < (int)len) {
      filter->output_buffer[i] = gain;
    }
  }

  // Forward pass: Apply exponential release to the gain envelope.
  for (size_t i = 0; i < len; i++) {
    // Release gain exponentially towards 1.0 using pow() in the log/dB domain.
    filter->release_gain = pow(filter->release_gain, filter->release_coeff);
    if (filter->output_buffer[i] < filter->release_gain) {
      // Instantaneous gain reduction if the peak requires it.
      filter->release_gain = filter->output_buffer[i];
    } else {
      // Apply the slower release gain.
      filter->output_buffer[i] = filter->release_gain;
    }
  }

  // Apply gain reduction: Multiply the delayed input samples by the computed
  // gain.
  for (size_t i = 0; i < len; i++) {
    double input_sample;
    if (i < (size_t)filter->attack_samples) {
      input_sample = get_occupied(filter, lookahead_start + i);
    } else {
      input_sample = waveform[i - filter->attack_samples];
    }
    filter->output_buffer[i] *= input_sample;
  }

  // Update lookahead buffer and Output:
  // Push the current raw input into the lookahead delay buffer,
  // and write the computed limited samples to the output waveform.
  for (size_t i = 0; i < len; i++) {
    push_overwrite(filter, waveform[i]);
    waveform[i] = filter->output_buffer[i];
  }
}

void lookahead_limiter_filter_process(lookahead_limiter_filter_t* filter,
                                      mutable_waveform_t waveform,
                                      size_t count) {
  if (!filter || !waveform || count == 0) return;
  size_t processed = 0;
  while (processed < count) {
    size_t slice = count - processed;
    if (slice > filter->output_buffer_capacity) {
      slice = filter->output_buffer_capacity;
    }
    process_slice(filter, waveform + processed, slice);
    processed += slice;
  }
}

void lookahead_limiter_filter_free(lookahead_limiter_filter_t* filter) {
  if (!filter) return;
  if (filter->lookahead_data) free(filter->lookahead_data);
  if (filter->output_buffer) free(filter->output_buffer);
  free(filter);
}
