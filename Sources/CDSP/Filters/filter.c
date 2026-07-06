#include "filter.h"

#include <stdlib.h>
#include <string.h>

/// Protocol for all audio filters. Filters operate on one channel at a time.
///
/// `waveform` is a pointer into class-owned storage (`AudioBuffers`). The
/// pointer's `count` is the number of samples to process — typically the
/// owning chunk's `validFrames`, sliced down by the caller. Filters must
/// not assume the pointer covers the channel's full capacity.

/// Factory to create filter instances from configuration.
///
/// Validation runs first via `FilterConfig.validate(sampleRate:)`; the
/// switch then constructs the runtime filter for each variant. The
/// `.volume` case is reserved for the implicit master-volume filter
/// inside `Pipeline` and cannot be user-defined.
filter_t* filter_create(const char* name, const filter_config_t* config,
                        int sample_rate, size_t chunk_size,
                        processing_parameters_t* proc_params) {
  if (!config) return NULL;
  filter_t* filter = (filter_t*)malloc(sizeof(filter_t));
  if (!filter) return NULL;
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "filter");
  }

  switch (config->type) {
    case FILTER_TYPE_BIQUAD: {
      biquad_coefficients_t coeffs;
      biquad_coefficients_compute(&config->parameters.biquad, sample_rate,
                                  &coeffs);
      filter->type = FILTER_INSTANCE_BIQUAD;
      filter->instance = biquad_filter_create(name, &coeffs);
      break;
    }
    case FILTER_TYPE_BIQUAD_COMBO:
      filter->type = FILTER_INSTANCE_BIQUAD_COMBO;
      filter->instance = biquad_combo_filter_create(
          name, &config->parameters.biquad_combo, sample_rate);
      break;
    case FILTER_TYPE_CONV:
      filter->type = FILTER_INSTANCE_CONVOLUTION;
      filter->instance =
          convolution_filter_create(name, &config->parameters.conv, chunk_size);
      break;
    case FILTER_TYPE_DELAY:
      filter->type = FILTER_INSTANCE_DELAY;
      filter->instance =
          delay_filter_create(name, &config->parameters.delay, sample_rate);
      break;
    case FILTER_TYPE_DIFF_EQ:
      filter->type = FILTER_INSTANCE_DIFF_EQ;
      filter->instance =
          diffeq_filter_create(name, &config->parameters.diff_eq);
      break;
    case FILTER_TYPE_DITHER:
      filter->type = FILTER_INSTANCE_DITHER;
      filter->instance = dither_filter_create(name, &config->parameters.dither);
      break;
    case FILTER_TYPE_GAIN:
      filter->type = FILTER_INSTANCE_GAIN;
      filter->instance = gain_filter_create(name, &config->parameters.gain);
      break;
    case FILTER_TYPE_LIMITER:
      filter->type = FILTER_INSTANCE_LIMITER;
      filter->instance =
          limiter_filter_create(name, &config->parameters.limiter);
      break;
    case FILTER_TYPE_LOOKAHEAD_LIMITER:
      filter->type = FILTER_INSTANCE_LOOKAHEAD_LIMITER;
      filter->instance = lookahead_limiter_filter_create(
          name, &config->parameters.lookahead_limiter, sample_rate, chunk_size);
      break;
    case FILTER_TYPE_LOUDNESS:
      filter->type = FILTER_INSTANCE_LOUDNESS;
      filter->instance = loudness_filter_create(
          name, &config->parameters.loudness, sample_rate, proc_params);
      break;
    case FILTER_TYPE_VOLUME:
      filter->type = FILTER_INSTANCE_VOLUME;
      filter->instance =
          volume_filter_create(name, &config->parameters.volume, sample_rate,
                               chunk_size, proc_params);
      break;
    default:
      free(filter);
      return NULL;
  }

  if (!filter->instance) {
    free(filter);
    return NULL;
  }
  return filter;
}

/// Process a waveform buffer in-place. The buffer's `count` defines the
/// processed range.
void filter_process(filter_t* filter, mutable_waveform_t waveform,
                    size_t count) {
  if (!filter || !waveform || count == 0 || !filter->instance) return;
  switch (filter->type) {
    case FILTER_INSTANCE_BIQUAD:
      biquad_filter_process((biquad_filter_t*)filter->instance, waveform,
                            count);
      break;
    case FILTER_INSTANCE_BIQUAD_COMBO:
      biquad_combo_filter_process((biquad_combo_filter_t*)filter->instance,
                                  waveform, count);
      break;
    case FILTER_INSTANCE_CONVOLUTION:
      convolution_filter_process((convolution_filter_t*)filter->instance,
                                 waveform, count);
      break;
    case FILTER_INSTANCE_DELAY:
      delay_filter_process((delay_filter_t*)filter->instance, waveform, count);
      break;
    case FILTER_INSTANCE_DIFF_EQ:
      diffeq_filter_process((diffeq_filter_t*)filter->instance, waveform,
                            count);
      break;
    case FILTER_INSTANCE_DITHER:
      dither_filter_process((dither_filter_t*)filter->instance, waveform,
                            count);
      break;
    case FILTER_INSTANCE_GAIN:
      gain_filter_process((gain_filter_t*)filter->instance, waveform, count);
      break;
    case FILTER_INSTANCE_LIMITER:
      limiter_filter_process((limiter_filter_t*)filter->instance, waveform,
                             count);
      break;
    case FILTER_INSTANCE_LOOKAHEAD_LIMITER:
      lookahead_limiter_filter_process(
          (lookahead_limiter_filter_t*)filter->instance, waveform, count);
      break;
    case FILTER_INSTANCE_LOUDNESS:
      loudness_filter_process((loudness_filter_t*)filter->instance, waveform,
                              count);
      break;
    case FILTER_INSTANCE_VOLUME: {
      volume_filter_t* vf = (volume_filter_t*)filter->instance;
      volume_filter_prepare_chunk(vf);
      volume_filter_process(vf, waveform, count);
      volume_filter_advance_ramp(vf);
      break;
    }
  }
}

/// Update the filter parameters dynamically.
void filter_update_parameters(filter_t* filter, const filter_config_t* config,
                              int sample_rate) {
  if (!filter || !config || !filter->instance) return;
  switch (filter->type) {
    case FILTER_INSTANCE_BIQUAD:
      biquad_filter_update_parameters((biquad_filter_t*)filter->instance,
                                      config, sample_rate);
      break;
    case FILTER_INSTANCE_BIQUAD_COMBO:
      biquad_combo_filter_update_parameters(
          (biquad_combo_filter_t*)filter->instance, config, sample_rate);
      break;
    case FILTER_INSTANCE_DELAY:
      delay_filter_update_parameters((delay_filter_t*)filter->instance, config,
                                     sample_rate);
      break;
    case FILTER_INSTANCE_DIFF_EQ:
      diffeq_filter_update_parameters((diffeq_filter_t*)filter->instance,
                                      config, sample_rate);
      break;
    case FILTER_INSTANCE_DITHER:
      dither_filter_update_parameters((dither_filter_t*)filter->instance,
                                      config, sample_rate);
      break;
    case FILTER_INSTANCE_GAIN:
      gain_filter_update_parameters((gain_filter_t*)filter->instance, config,
                                    sample_rate);
      break;
    case FILTER_INSTANCE_LIMITER:
      limiter_filter_update_parameters((limiter_filter_t*)filter->instance,
                                       config, sample_rate);
      break;
    case FILTER_INSTANCE_LOOKAHEAD_LIMITER:
      lookahead_limiter_filter_update_parameters(
          (lookahead_limiter_filter_t*)filter->instance, config, sample_rate);
      break;
    case FILTER_INSTANCE_LOUDNESS:
      loudness_filter_update_parameters((loudness_filter_t*)filter->instance,
                                        config, sample_rate);
      break;
    case FILTER_INSTANCE_VOLUME:
      volume_filter_update_parameters((volume_filter_t*)filter->instance,
                                      config, sample_rate);
      break;
    default:
      break;
  }
}

void filter_free(filter_t* filter) {
  if (!filter) return;
  if (filter->instance) {
    switch (filter->type) {
      case FILTER_INSTANCE_BIQUAD:
        biquad_filter_free((biquad_filter_t*)filter->instance);
        break;
      case FILTER_INSTANCE_BIQUAD_COMBO:
        biquad_combo_filter_free((biquad_combo_filter_t*)filter->instance);
        break;
      case FILTER_INSTANCE_CONVOLUTION:
        convolution_filter_free((convolution_filter_t*)filter->instance);
        break;
      case FILTER_INSTANCE_DELAY:
        delay_filter_free((delay_filter_t*)filter->instance);
        break;
      case FILTER_INSTANCE_DIFF_EQ:
        diffeq_filter_free((diffeq_filter_t*)filter->instance);
        break;
      case FILTER_INSTANCE_DITHER:
        dither_filter_free((dither_filter_t*)filter->instance);
        break;
      case FILTER_INSTANCE_GAIN:
        gain_filter_free((gain_filter_t*)filter->instance);
        break;
      case FILTER_INSTANCE_LIMITER:
        limiter_filter_free((limiter_filter_t*)filter->instance);
        break;
      case FILTER_INSTANCE_LOOKAHEAD_LIMITER:
        lookahead_limiter_filter_free(
            (lookahead_limiter_filter_t*)filter->instance);
        break;
      case FILTER_INSTANCE_LOUDNESS:
        loudness_filter_free((loudness_filter_t*)filter->instance);
        break;
      case FILTER_INSTANCE_VOLUME:
        volume_filter_free((volume_filter_t*)filter->instance);
        break;
    }
  }
  free(filter);
}
