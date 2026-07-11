// Concurrency model
// -----------------
// Every field is backed by lock-free atomics (`atomic_double_t` or `_Atomic
// bool`) — no mutexes or locks.
#include "Audio/processing_parameters.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef __APPLE__
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
/**
 * @brief Helper to get the current time in nanoseconds.
 *
 * Fallback implementation for non-Apple platforms using clock_gettime.
 *
 * @param clock_id The clock identifier (e.g., CLOCK_MONOTONIC).
 * @return Current time in nanoseconds.
 */
static inline uint64_t clock_gettime_nsec_np(int clock_id) {
  struct timespec ts;
  clock_gettime(clock_id, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
#endif

processing_parameters_t* processing_parameters_create(
    size_t capture_channels, size_t playback_channels) {
  processing_parameters_t* params =
      (processing_parameters_t*)calloc(1, sizeof(processing_parameters_t));
  if (!params) return NULL;

  for (int i = 0; i < FADER_COUNT; i++) {
    atomic_double_init(&params->target_volumes[i],
                       PROCESSING_PARAMETERS_DEFAULT_VOLUME);
    atomic_init(&params->target_volume_set_at[i], 0ULL);
    atomic_double_init(&params->current_volumes[i],
                       PROCESSING_PARAMETERS_DEFAULT_VOLUME);
    atomic_init(&params->muted[i], PROCESSING_PARAMETERS_DEFAULT_MUTE);
  }

  params->capture_channels = capture_channels;
  params->playback_channels = playback_channels;

  if (capture_channels > 0) {
    params->capture_signal_peak =
        (atomic_double_t*)malloc(capture_channels * sizeof(atomic_double_t));
    params->capture_signal_rms =
        (atomic_double_t*)malloc(capture_channels * sizeof(atomic_double_t));
    if (!params->capture_signal_peak || !params->capture_signal_rms) {
      processing_parameters_free(params);
      return NULL;
    }
    for (size_t i = 0; i < capture_channels; i++) {
      atomic_double_init(&params->capture_signal_peak[i], -1000.0);
      atomic_double_init(&params->capture_signal_rms[i], -1000.0);
    }
  }

  if (playback_channels > 0) {
    params->playback_signal_peak =
        (atomic_double_t*)malloc(playback_channels * sizeof(atomic_double_t));
    params->playback_signal_rms =
        (atomic_double_t*)malloc(playback_channels * sizeof(atomic_double_t));
    if (!params->playback_signal_peak || !params->playback_signal_rms) {
      processing_parameters_free(params);
      return NULL;
    }
    for (size_t i = 0; i < playback_channels; i++) {
      atomic_double_init(&params->playback_signal_peak[i], -1000.0);
      atomic_double_init(&params->playback_signal_rms[i], -1000.0);
    }
  }

  atomic_double_init(&params->rate_adjust, 1.0);
  atomic_double_init(&params->buffer_level, 0.0);
  atomic_init(&params->clipped_samples, 0ULL);
  atomic_double_init(&params->processing_load, 0.0);
  atomic_double_init(&params->resampler_load, 0.0);

  return params;
}

void processing_parameters_free(processing_parameters_t* params) {
  if (!params) return;
  if (params->capture_signal_peak) free(params->capture_signal_peak);
  if (params->capture_signal_rms) free(params->capture_signal_rms);
  if (params->playback_signal_peak) free(params->playback_signal_peak);
  if (params->playback_signal_rms) free(params->playback_signal_rms);
  free(params);
}

double processing_parameters_get_target_volume_for_fader(
    const processing_parameters_t* params, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT) return 0.0;
  return atomic_double_get(&params->target_volumes[fader]);
}

void processing_parameters_set_target_volume_for_fader(
    processing_parameters_t* params, double value, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT) return;
  atomic_double_set(&params->target_volumes[fader], value);
  uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  atomic_store_explicit(&params->target_volume_set_at[fader], now,
                        memory_order_release);
}

uint64_t processing_parameters_get_target_volume_set_at_for_fader(
    const processing_parameters_t* params, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT) return 0ULL;
  return atomic_load_explicit(&params->target_volume_set_at[fader],
                              memory_order_acquire);
}

double processing_parameters_get_current_volume_for_fader(
    const processing_parameters_t* params, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT) return 0.0;
  return atomic_double_get(&params->current_volumes[fader]);
}

void processing_parameters_set_current_volume_for_fader(
    processing_parameters_t* params, double value, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT) return;
  atomic_double_set(&params->current_volumes[fader], value);
}

bool processing_parameters_is_muted_for_fader(
    const processing_parameters_t* params, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT) return false;
  return atomic_load_explicit(&params->muted[fader], memory_order_acquire);
}

void processing_parameters_set_muted_for_fader(processing_parameters_t* params,
                                               bool value, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT) return;
  atomic_store_explicit(&params->muted[fader], value, memory_order_release);
}

void processing_parameters_get_capture_signal_peak(
    const processing_parameters_t* params, double* out_levels, size_t count) {
  if (!params || !out_levels || !params->capture_signal_peak) return;
  size_t limit =
      count < params->capture_channels ? count : params->capture_channels;
  for (size_t i = 0; i < limit; i++) {
    out_levels[i] = atomic_double_get(&params->capture_signal_peak[i]);
  }
}

void processing_parameters_set_capture_signal_peak(
    processing_parameters_t* params, const double* levels, size_t count) {
  if (!params || !levels || !params->capture_signal_peak) return;
  size_t limit =
      count < params->capture_channels ? count : params->capture_channels;
  for (size_t i = 0; i < limit; i++) {
    atomic_double_set(&params->capture_signal_peak[i], levels[i]);
  }
}

void processing_parameters_get_capture_signal_rms(
    const processing_parameters_t* params, double* out_levels, size_t count) {
  if (!params || !out_levels || !params->capture_signal_rms) return;
  size_t limit =
      count < params->capture_channels ? count : params->capture_channels;
  for (size_t i = 0; i < limit; i++) {
    out_levels[i] = atomic_double_get(&params->capture_signal_rms[i]);
  }
}

void processing_parameters_set_capture_signal_rms(
    processing_parameters_t* params, const double* levels, size_t count) {
  if (!params || !levels || !params->capture_signal_rms) return;
  size_t limit =
      count < params->capture_channels ? count : params->capture_channels;
  for (size_t i = 0; i < limit; i++) {
    atomic_double_set(&params->capture_signal_rms[i], levels[i]);
  }
}

void processing_parameters_get_playback_signal_peak(
    const processing_parameters_t* params, double* out_levels, size_t count) {
  if (!params || !out_levels || !params->playback_signal_peak) return;
  size_t limit =
      count < params->playback_channels ? count : params->playback_channels;
  for (size_t i = 0; i < limit; i++) {
    out_levels[i] = atomic_double_get(&params->playback_signal_peak[i]);
  }
}

void processing_parameters_set_playback_signal_peak(
    processing_parameters_t* params, const double* levels, size_t count) {
  if (!params || !levels || !params->playback_signal_peak) return;
  size_t limit =
      count < params->playback_channels ? count : params->playback_channels;
  for (size_t i = 0; i < limit; i++) {
    atomic_double_set(&params->playback_signal_peak[i], levels[i]);
  }
}

void processing_parameters_get_playback_signal_rms(
    const processing_parameters_t* params, double* out_levels, size_t count) {
  if (!params || !out_levels || !params->playback_signal_rms) return;
  size_t limit =
      count < params->playback_channels ? count : params->playback_channels;
  for (size_t i = 0; i < limit; i++) {
    out_levels[i] = atomic_double_get(&params->playback_signal_rms[i]);
  }
}

void processing_parameters_set_playback_signal_rms(
    processing_parameters_t* params, const double* levels, size_t count) {
  if (!params || !levels || !params->playback_signal_rms) return;
  size_t limit =
      count < params->playback_channels ? count : params->playback_channels;
  for (size_t i = 0; i < limit; i++) {
    atomic_double_set(&params->playback_signal_rms[i], levels[i]);
  }
}

/**
 * @brief Lock-free helper to update audio levels (Peak and RMS) for each
 * channel.
 *
 * Calculates peak and RMS values in dB for the active channels in the chunk
 * and updates the respective atomic storage. It avoids dynamic allocation,
 * making it suitable for the audio processing thread.
 *
 * @param chunk The audio chunk to process.
 * @param peak_storage Atomic storage array for peak levels.
 * @param rms_storage Atomic storage array for RMS levels.
 * @param storage_count Capacity of the storage arrays.
 * @return The maximum peak level (dB) across all processed channels.
 */
static double update_levels_internal(const audio_chunk_t* chunk,
                                     atomic_double_t* peak_storage,
                                     atomic_double_t* rms_storage,
                                     size_t storage_count) {
  if (!chunk || !peak_storage || !rms_storage) return -1000.0;
  size_t chunk_channels = audio_chunk_get_channels(chunk);
  size_t channel_count =
      chunk_channels < storage_count ? chunk_channels : storage_count;
  if (channel_count == 0) return -1000.0;
  size_t frame_count = audio_chunk_get_valid_frames(chunk);
  double max_peak = -1000.0;

  for (size_t i = 0; i < channel_count; i++) {
    waveform_t buffer = audio_chunk_get_channel(chunk, i);
    if (!buffer) continue;

    double peak_db = double_to_db(dsp_ops_peak_absolute(buffer, frame_count));
    atomic_double_set(&peak_storage[i], peak_db);
    if (peak_db > max_peak) {
      max_peak = peak_db;
    }

    double rms_db = double_to_db(dsp_ops_rms(buffer, frame_count));
    atomic_double_set(&rms_storage[i], rms_db);
  }
  return max_peak;
}

double processing_parameters_update_capture_levels(
    processing_parameters_t* params, const audio_chunk_t* chunk) {
  if (!params) return -1000.0;
  return update_levels_internal(chunk, params->capture_signal_peak,
                                params->capture_signal_rms,
                                params->capture_channels);
}

double processing_parameters_update_playback_levels(
    processing_parameters_t* params, const audio_chunk_t* chunk) {
  if (!params) return -1000.0;
  return update_levels_internal(chunk, params->playback_signal_peak,
                                params->playback_signal_rms,
                                params->playback_channels);
}
