#include "volume.h"

struct volume_filter {
  char name[64];
  fader_t fader;
  double volume_limit;
  size_t chunk_size;
  int ramptime_in_chunks;
  uint64_t stale_ramp_threshold_ns;
  double current_volume;
  double target_volume;
  double target_linear_gain;
  bool mute;
  double ramp_start;
  int ramp_step;
  double* current_ramp_gains;
  processing_parameters_t* processing_parameters;
};

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef __APPLE__
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
/**
 * @brief Helper to get the current time in nanoseconds.
 *
 * Used for checking if volume ramp requests are stale.
 *
 * @param clock_id The clock ID to query.
 * @return The current time in nanoseconds.
 */
static inline uint64_t clock_gettime_nsec_np(int clock_id) {
  struct timespec ts;
  clock_gettime(clock_id, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
#endif

/**
 * @brief Pre-calculates the gain values for the current step of a volume ramp.
 *
 * This function calculates linear gain coefficients for each sample in the
 * current chunk to smoothly transition from the start volume to the target
 * volume over the ramp duration.
 *
 * @param filter Pointer to the volume filter instance.
 */
static void fill_ramp(volume_filter_t* filter) {
  if (filter->chunk_size == 0 || filter->ramptime_in_chunks <= 0) return;
  double target_vol = filter->mute ? -100.0 : filter->target_volume;
  double ramprange =
      (target_vol - filter->ramp_start) / (double)filter->ramptime_in_chunks;
  double stepsize = ramprange / (double)filter->chunk_size;
  for (size_t val = 0; val < filter->chunk_size; val++) {
    double db_val = filter->ramp_start +
                    ramprange * ((double)filter->ramp_step - 1.0) +
                    (double)val * stepsize;
    filter->current_ramp_gains[val] = double_from_db(db_val);
  }
}

volume_filter_t* volume_filter_create(const char* name,
                                      const volume_parameters_t* params,
                                      int sample_rate, size_t chunk_size,
                                      processing_parameters_t* proc_params,
                                      config_error_t* err) {
  if (sample_rate <= 0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER, "VolumeFilter: sample_rate must be positive");
    return NULL;
  }
  if (chunk_size == 0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER, "VolumeFilter: chunk_size must be positive");
    return NULL;
  }
  volume_filter_t* filter = (volume_filter_t*)calloc(1, sizeof(volume_filter_t));
  if (!filter) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate volume filter wrapper");
    return NULL;
  }
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "volume");
  }
  filter->fader = params ? params->fader : FADER_MAIN;
  double ramp_time_ms =
      (params && params->has_ramp_time) ? params->ramp_time : 400.0;
  filter->volume_limit = (params && params->has_limit) ? params->limit : 50.0;
  filter->chunk_size = chunk_size;
  filter->processing_parameters = proc_params;

  filter->ramptime_in_chunks = (int)round(
      ramp_time_ms / (1000.0 * (double)chunk_size / (double)sample_rate));
  filter->stale_ramp_threshold_ns =
      1500000000ULL * (uint64_t)chunk_size / (uint64_t)sample_rate;
  // Pre-allocate array
  filter->current_ramp_gains =
      (double*)calloc(chunk_size > 0 ? chunk_size : 1, sizeof(double));
  if (!filter->current_ramp_gains) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate volume fader ramp gains array");
    volume_filter_free(filter);
    return NULL;
  }

  // Initialize state from shared parameters to prevent volume burst on startup
  double initial_vol = proc_params
                           ? processing_parameters_get_target_volume_for_fader(
                                 proc_params, filter->fader)
                           : 0.0;
  bool initial_mute = proc_params ? processing_parameters_is_muted_for_fader(
                                        proc_params, filter->fader)
                                  : false;
  double initial_vol_clamped =
      initial_vol < filter->volume_limit ? initial_vol : filter->volume_limit;

  filter->target_volume = initial_vol_clamped;
  filter->mute = initial_mute;
  filter->current_volume = initial_mute ? -100.0 : initial_vol_clamped;
  filter->target_linear_gain =
      initial_mute ? 0.0 : double_from_db(initial_vol_clamped);
  filter->ramp_start = filter->current_volume;
  filter->ramp_step = 0;

  return filter;
}

/// Pre-calculates target volume levels and generates ramping array once per
/// chunk. Must be called once per audio chunk before processing individual
/// channel waveforms.
void volume_filter_prepare_chunk(volume_filter_t* filter) {
  if (!filter || !filter->processing_parameters) return;
  double shared_vol = processing_parameters_get_target_volume_for_fader(
      filter->processing_parameters, filter->fader);
  bool shared_mute = processing_parameters_is_muted_for_fader(
      filter->processing_parameters, filter->fader);
  double target_vol =
      shared_vol < filter->volume_limit ? shared_vol : filter->volume_limit;

  if (fabs(target_vol - filter->target_volume) > 0.01 ||
      filter->mute != shared_mute) {
    uint64_t set_at = processing_parameters_get_target_volume_set_at_for_fader(
        filter->processing_parameters, filter->fader);
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    // Determine if the volume change request is stale.
    // If the request is stale (e.g. after a long pause or block), we skip the
    // ramp to prevent volume changes from lagging behind user interaction.
    bool ramp_is_stale =
        (now > set_at) ? ((now - set_at) > filter->stale_ramp_threshold_ns)
                       : false;

    if (filter->ramptime_in_chunks > 0 && !ramp_is_stale) {
      filter->ramp_start = filter->current_volume;
      filter->ramp_step = 1;
    } else {
      filter->current_volume = shared_mute ? -100.0 : target_vol;
      filter->ramp_step = 0;
    }
    filter->target_volume = target_vol;
    filter->target_linear_gain = shared_mute ? 0.0 : double_from_db(target_vol);
    filter->mute = shared_mute;
  }

  if (filter->ramp_step > 0 &&
      filter->ramp_step <= filter->ramptime_in_chunks) {
    fill_ramp(filter);
  }
}

/// Conforms to `Filter`. Processes a single channel's waveform slice.
void volume_filter_process(volume_filter_t* filter, mutable_waveform_t waveform,
                           size_t count) {
  if (!filter || !waveform || count == 0) return;
  if (filter->ramp_step == 0) {
    // Optimization: avoid multiplication if gain is 1.0, or clear if 0.0.
    if (filter->target_linear_gain == 1.0) {
      // No-op
    } else if (filter->target_linear_gain == 0.0) {
      dsp_ops_clear(waveform, count);
    } else {
      dsp_ops_scalar_multiply(waveform, filter->target_linear_gain, count);
    }
  } else {
    // Apply the ramping gains.
    size_t limit = count < filter->chunk_size ? count : filter->chunk_size;
    dsp_ops_multiply(filter->current_ramp_gains, waveform, limit);
    // If there is leftover data in the chunk beyond the ramping buffer,
    // apply the target linear gain to it.
    if (limit < count) {
      double final_gain =
          filter->mute ? 0.0 : double_from_db(filter->target_volume);
      dsp_ops_scalar_multiply(waveform + limit, final_gain, count - limit);
    }
  }
}

/// Advances the fader's ramp steps.
/// Must be called once per audio chunk after all channels have been processed.
void volume_filter_advance_ramp(volume_filter_t* filter) {
  if (!filter || filter->ramp_step <= 0) return;
  if (filter->chunk_size > 0) {
    // Update current volume based on the last computed gain sample of the
    // chunk. Clamp to a tiny value to prevent log10(0) returning -inf.
    double last_gain = filter->current_ramp_gains[filter->chunk_size - 1];
    double val = last_gain > 1e-150 ? last_gain : 1e-150;
    filter->current_volume = 20.0 * log10(val);
  }
  filter->ramp_step++;
  if (filter->ramp_step > filter->ramptime_in_chunks) {
    filter->ramp_step = 0;
  }
  if (filter->processing_parameters) {
    processing_parameters_set_current_volume_for_fader(
        filter->processing_parameters, filter->current_volume, filter->fader);
  }
}

void volume_filter_transfer_state(volume_filter_t* dest,
                                  const volume_filter_t* src) {
  if (!dest || !src) return;
  dest->current_volume = src->current_volume;
  dest->target_volume = src->target_volume;
  dest->target_linear_gain = src->target_linear_gain;
  dest->mute = src->mute;
  dest->ramp_start = src->ramp_start;
  dest->ramp_step = src->ramp_step;
  if (dest->chunk_size == src->chunk_size && dest->current_ramp_gains &&
      src->current_ramp_gains) {
    memcpy(dest->current_ramp_gains, src->current_ramp_gains,
           dest->chunk_size * sizeof(double));
  }
}

void volume_filter_free(volume_filter_t* filter) {
  if (!filter) return;
  if (filter->current_ramp_gains) free(filter->current_ramp_gains);
  free(filter);
}
