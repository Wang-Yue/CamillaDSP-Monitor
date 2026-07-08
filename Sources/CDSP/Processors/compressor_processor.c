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
    free(processor);
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

  // Step 1: Sum monitored channels into scratch buffer to evaluate overall
  // signal level (creating a mono sum for sidechain level detection).
  int ch0 = processor->monitor_channels[0];
  const double* src0_base = audio_chunk_get_channel(chunk, ch0);
  if (!src0_base) return;
  memcpy(processor->scratch, src0_base, count * sizeof(double));

  for (size_t ch_idx = 1; ch_idx < processor->monitor_channels_count;
       ch_idx++) {
    int ch = processor->monitor_channels[ch_idx];
    const double* src_base = audio_chunk_get_channel(chunk, ch);
    if (!src_base) continue;
    // Perform vector addition to sum the channel's samples into scratch.
#ifdef ENABLE_ACCELERATE
    vDSP_vaddD(processor->scratch, 1, src_base, 1, processor->scratch, 1,
               count);
#else
    for (size_t i = 0; i < count; i++) {
      processor->scratch[i] += src_base[i];
    }
#endif
  }

  // Step 2: Envelope Detection (Loudness Estimation with Attack/Release
  // Smoothing)
  // We process sample-by-sample, smoothing the loudness envelope in dB.
  double prev = processor->prev_loudness;
  for (size_t i = 0; i < count; i++) {
    // Convert absolute amplitude to dB. 1e-9 avoids log10(0) which is -inf.
    double val = 20.0 * log10(fabs(processor->scratch[i]) + 1e-9);
    if (val >= prev) {
      // Signal level rising: apply attack time constant.
      // attack coefficient determines how quickly the envelope responds to
      // level increases.
      val = processor->attack * prev + (1.0 - processor->attack) * val;
    } else {
      // Signal level falling: apply release time constant.
      // release coefficient determines how slowly the envelope decays back
      // down.
      val = processor->release * prev + (1.0 - processor->release) * val;
    }
    prev = val;
    processor->scratch[i] = val;
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
  for (size_t ch_idx = 0; ch_idx < processor->process_channels_count;
       ch_idx++) {
    int ch = processor->process_channels[ch_idx];
    double* wave = audio_chunk_get_channel(chunk, ch);
    if (!wave) continue;
#ifdef ENABLE_ACCELERATE
    vDSP_vmulD(wave, 1, processor->scratch, 1, wave, 1, count);
#else
    for (size_t i = 0; i < count; i++) {
      wave[i] *= processor->scratch[i];
    }
#endif
    // Step 5: Optionally run post-compression limiter to prevent clipping
    if (processor->limiter) {
      limiter_filter_process(processor->limiter, wave, count);
    }
  }
}

void compressor_processor_update_parameters(compressor_processor_t* processor,
                                            const processor_config_t* config,
                                            int sample_rate) {
  if (!processor || !config || sample_rate <= 0) return;
  if (config->type != PROCESSOR_TYPE_COMPRESSOR) return;
  const compressor_parameters_t* params = &config->parameters.compressor;

  if (params->monitor_channels_count > 0 && params->monitor_channels) {
    free(processor->monitor_channels);
    processor->monitor_channels_count = params->monitor_channels_count;
    processor->monitor_channels =
        (int*)calloc(processor->monitor_channels_count, sizeof(int));
    memcpy(processor->monitor_channels, params->monitor_channels,
           processor->monitor_channels_count * sizeof(int));
  }
  if (params->process_channels_count > 0 && params->process_channels) {
    free(processor->process_channels);
    processor->process_channels_count = params->process_channels_count;
    processor->process_channels =
        (int*)calloc(processor->process_channels_count, sizeof(int));
    memcpy(processor->process_channels, params->process_channels,
           processor->process_channels_count * sizeof(int));
  }

  double srate = (double)sample_rate;
  processor->attack = exp(-1.0 / srate / params->attack);
  processor->release = exp(-1.0 / srate / params->release);
  processor->threshold = params->threshold;
  processor->factor = params->factor;
  processor->makeup_gain = params->has_makeup_gain ? params->makeup_gain : 0.0;

  if (params->has_clip_limit) {
    limiter_parameters_t limit_params = {0};
    limit_params.clip_limit = params->clip_limit;
    limit_params.soft_clip = params->soft_clip;
    if (processor->limiter) {
      filter_config_t fconfig = {0};
      fconfig.type = FILTER_TYPE_LIMITER;
      fconfig.parameters.limiter = limit_params;
      limiter_filter_update_parameters(processor->limiter, &fconfig,
                                       sample_rate);
    } else {
      processor->limiter = limiter_filter_create("limiter", &limit_params);
    }
  } else {
    if (processor->limiter) {
      limiter_filter_free(processor->limiter);
      processor->limiter = NULL;
    }
  }
}
