/**
 * @file compressor_processor.c
 * @brief Implementation of the dynamic range compressor processor.
 *
 * Implementation details:
 * - Exponential smoothing coefficients attack and release are precomputed as:
 *   attack = exp(-1.0 / (sample_rate * attack_time))
 *   release = exp(-1.0 / (sample_rate * release_time))
 * - Real-time processing (`compressor_processor_process`):
 *   1. Sums monitored channel waveforms into a pre-allocated scratch buffer
 * using vDSP_vaddD (Apple Accelerate) or scalar addition.
 *   2. Envelope Detection: Computes instantaneous dB loudness and smooths it
 * using attack filter when level rises, and release filter when level falls.
 *   3. Gain Reduction Curve: Applies compression ratio factor above threshold:
 * -(val - threshold) * (factor - 1.0) / factor, adds makeup gain, and converts
 * from dB to linear gain.
 *   4. Multiplies processed channels by linear gain curve using vDSP_vmulD
 * (Apple Accelerate) or scalar multiplication.
 *   5. Optionally applies post-compression limiter filter.
 */

#include "compressor_processor.h"
#include "Logging/app_logger.h"

struct compressor_processor {
  char name[64];          ///< Unique name of the compressor instance.
  int* monitor_channels;  ///< Array of channel indices to monitor for level
                          ///< detection.
  size_t monitor_channels_count;  ///< Number of monitored channels.
  int* process_channels;  ///< Array of channel indices to apply gain reduction
                          ///< to.
  size_t process_channels_count;  ///< Number of processed channels.
  double attack;       ///< Exponential smoothing coefficient for attack phase.
  double release;      ///< Exponential smoothing coefficient for release phase.
  double threshold;    ///< Compression threshold in dB.
  double factor;       ///< Compression ratio factor (e.g., 4.0 for 4:1).
  double makeup_gain;  ///< Post-compression makeup gain in dB.
  limiter_filter_t*
      limiter;  ///< Optional peak/soft limiter applied after compression.
  double*
      scratch;  ///< Pre-allocated scratch buffer for envelope/gain calculation.
  size_t scratch_capacity;  ///< Capacity of scratch buffer in frames (matches
                            ///< chunk_size).
  double prev_loudness;     ///< State variable storing envelope loudness from
                            ///< previous sample.
  bool channel_warning_logged; ///< Track if we already logged a channel mismatch warning.
};

const char* compressor_processor_get_name(
    const compressor_processor_t* processor) {
  return processor ? processor->name : "";
}

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ENABLE_ACCELERATE
#include <Accelerate/Accelerate.h>
#endif

compressor_processor_t* compressor_processor_create(
    const char* name, const compressor_parameters_t* params, int sample_rate,
    size_t chunk_size) {
  if (!params || sample_rate <= 0 || chunk_size == 0) return NULL;

  compressor_processor_t* processor =
      (compressor_processor_t*)calloc(1, sizeof(compressor_processor_t));
  if (!processor) return NULL;

  if (name) {
    strncpy(processor->name, name, sizeof(processor->name) - 1);
    processor->name[sizeof(processor->name) - 1] = '\0';
  } else {
    strcpy(processor->name, "compressor");
  }

  processor->scratch_capacity = chunk_size;
  processor->scratch = (double*)calloc(chunk_size, sizeof(double));
  if (!processor->scratch) {
    compressor_processor_free(processor);
    return NULL;
  }

  if (params->monitor_channels_count > 0 && params->monitor_channels) {
    processor->monitor_channels_count = params->monitor_channels_count;
    processor->monitor_channels =
        (int*)calloc(processor->monitor_channels_count, sizeof(int));
    memcpy(processor->monitor_channels, params->monitor_channels,
           processor->monitor_channels_count * sizeof(int));
  } else {
    processor->monitor_channels_count = (size_t)params->channels;
    processor->monitor_channels =
        (int*)calloc(processor->monitor_channels_count, sizeof(int));
    for (size_t i = 0; i < processor->monitor_channels_count; i++) {
      processor->monitor_channels[i] = (int)i;
    }
  }

  if (params->process_channels_count > 0 && params->process_channels) {
    processor->process_channels_count = params->process_channels_count;
    processor->process_channels =
        (int*)calloc(processor->process_channels_count, sizeof(int));
    memcpy(processor->process_channels, params->process_channels,
           processor->process_channels_count * sizeof(int));
  } else {
    processor->process_channels_count = (size_t)params->channels;
    processor->process_channels =
        (int*)calloc(processor->process_channels_count, sizeof(int));
    for (size_t i = 0; i < processor->process_channels_count; i++) {
      processor->process_channels[i] = (int)i;
    }
  }

  if (!processor->monitor_channels || !processor->process_channels) {
    compressor_processor_free(processor);
    return NULL;
  }

  double srate = (double)sample_rate;
  processor->attack = exp(-1.0 / srate / params->attack);
  processor->release = exp(-1.0 / srate / params->release);
  processor->threshold = params->threshold;
  processor->factor = params->factor;
  processor->makeup_gain = params->has_makeup_gain ? params->makeup_gain : 0.0;
  processor->prev_loudness = -100.0;

  if (params->has_clip_limit) {
    limiter_parameters_t limit_params = {0};
    limit_params.clip_limit = params->clip_limit;
    limit_params.soft_clip = params->soft_clip;
    processor->limiter = limiter_filter_create("limiter", &limit_params);
    if (!processor->limiter) {
      compressor_processor_free(processor);
      return NULL;
    }
  } else {
    processor->limiter = NULL;
  }

  return processor;
}

void compressor_processor_free(compressor_processor_t* processor) {
  if (!processor) return;
  free(processor->monitor_channels);
  free(processor->process_channels);
  free(processor->scratch);
  if (processor->limiter) limiter_filter_free(processor->limiter);
  free(processor);
}

void compressor_processor_process(compressor_processor_t* processor,
                                  audio_chunk_t* chunk) {
  if (!processor || !chunk || !processor->scratch) return;
  size_t count = audio_chunk_get_valid_frames(chunk);
  if (count > processor->scratch_capacity) count = processor->scratch_capacity;
  if (count == 0 || processor->monitor_channels_count == 0) return;

  size_t ch_count = audio_chunk_get_channels(chunk);
  bool mismatch = false;
  for (size_t i = 0; i < processor->monitor_channels_count; i++) {
    if (processor->monitor_channels[i] < 0 || (size_t)processor->monitor_channels[i] >= ch_count) {
      mismatch = true;
      break;
    }
  }
  if (!mismatch) {
    for (size_t i = 0; i < processor->process_channels_count; i++) {
      if (processor->process_channels[i] < 0 || (size_t)processor->process_channels[i] >= ch_count) {
        mismatch = true;
        break;
      }
    }
  }
  if (mismatch) {
    if (!processor->channel_warning_logged) {
      logger_t logger = logger_create("compressor_processor");
      logger_error(&logger, "Compressor channel indices out of bounds for chunk channels (%d)",
                   log_arg_int((int64_t)ch_count),
                   log_arg_none(), log_arg_none(), log_arg_none());
      processor->channel_warning_logged = true;
    }
    return;
  }

  // Step 1: Sum monitored channels into scratch buffer to evaluate overall
  // signal level (creating a mono sum for sidechain level detection).
  audio_chunk_sum_channels(chunk, processor->monitor_channels,
                           processor->monitor_channels_count,
                           processor->scratch, count);

  // Step 2: Envelope Detection (Loudness Estimation with Attack/Release
  // Smoothing)
  double prev = processor->prev_loudness;
  for (size_t i = 0; i < count; i++) {
    double val = 20.0 * log10(fabs(processor->scratch[i]) + 1e-9);
    prev = double_smooth_envelope(val, prev, processor->attack,
                                  processor->release);
    processor->scratch[i] = prev;
  }
  // Store final envelope level for the next chunk's processing.
  processor->prev_loudness = prev;

  // Step 3: Gain Reduction Curve Calculation
  // Calculate the gain multiplier (in linear scale) for each sample based on
  // the envelope.
  for (size_t i = 0; i < count; i++) {
    double val = processor->scratch[i];
    if (val > processor->threshold) {
      // Above threshold: attenuate according to compression ratio (factor).
      // The attenuation in dB is: -(excess_dB * (ratio - 1) / ratio).
      val = -(val - processor->threshold) * (processor->factor - 1.0) /
            processor->factor;
    } else {
      // Below threshold: unity gain (0.0 dB reduction).
      val = 0.0;
    }
    // Apply user-defined makeup gain (in dB).
    val += processor->makeup_gain;
    // Convert gain reduction in dB to linear gain multiplier.
    processor->scratch[i] = double_from_db(val);
  }

  // Step 4: Apply linear gain to all processed channels
  audio_chunk_apply_gain(chunk, processor->process_channels,
                         processor->process_channels_count, processor->scratch,
                         count);

  // Step 5: Optionally run post-compression limiter to prevent clipping
  if (processor->limiter) {
    for (size_t ch_idx = 0; ch_idx < processor->process_channels_count;
         ch_idx++) {
      int ch = processor->process_channels[ch_idx];
      double* wave = audio_chunk_get_channel(chunk, ch);
      if (wave) {
        limiter_filter_process(processor->limiter, wave, count);
      }
    }
  }
}

void compressor_processor_transfer_state(compressor_processor_t* dest,
                                         const compressor_processor_t* src) {
  if (!dest || !src) return;
  dest->prev_loudness = src->prev_loudness;
}
