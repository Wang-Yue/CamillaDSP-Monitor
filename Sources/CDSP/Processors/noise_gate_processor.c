/**
 * @file noise_gate_processor.c
 * @brief Implementation of the noise gate processor.
 *
 * Implementation details:
 * - Real-time processing (`noise_gate_processor_process`):
 *   1. Sums monitored channels into scratch buffer using vDSP_vaddD or scalar
 * loop.
 *   2. Envelope Detection: Computes instantaneous dB loudness and smooths it
 * using attack filter when level rises, and release filter when level falls.
 *   3. Gate Threshold Logic: If loudness is below threshold, sets scratch
 * buffer gain to precomputed linear attenuation factor; otherwise sets gain
 * to 1.0 (unity).
 *   4. Multiplies processed channel waveforms by the computed gain curve using
 * vDSP_vmulD or scalar loop.
 */

#include "noise_gate_processor.h"

struct noise_gate_processor {
  char name[64];          ///< Unique name of the noise gate instance.
  int* monitor_channels;  ///< Array of channel indices monitored for level
                          ///< detection.
  size_t monitor_channels_count;  ///< Number of monitored channels.
  int* process_channels;  ///< Array of channel indices to apply gating to.
  size_t process_channels_count;  ///< Number of processed channels.
  double attack;     ///< Exponential smoothing coefficient for attack phase.
  double release;    ///< Exponential smoothing coefficient for release phase.
  double threshold;  ///< Gating threshold in dB.
  double factor;     ///< Linear attenuation gain applied when gate is closed.
  double* scratch;   ///< Pre-allocated scratch buffer for level detection.
  size_t scratch_capacity;  ///< Capacity of scratch buffer in frames.
  double prev_loudness;  ///< State variable tracking previous sample envelope
                         ///< loudness.
};

const char* noise_gate_processor_get_name(
    const noise_gate_processor_t* processor) {
  return processor ? processor->name : "";
}

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ENABLE_ACCELERATE
#include <Accelerate/Accelerate.h>
#endif

noise_gate_processor_t* noise_gate_processor_create(
    const char* name, const noise_gate_parameters_t* params, int sample_rate,
    size_t chunk_size) {
  if (!params || sample_rate <= 0 || chunk_size == 0) return NULL;

  noise_gate_processor_t* processor =
      (noise_gate_processor_t*)calloc(1, sizeof(noise_gate_processor_t));
  if (!processor) return NULL;

  if (name) {
    strncpy(processor->name, name, sizeof(processor->name) - 1);
    processor->name[sizeof(processor->name) - 1] = '\0';
  } else {
    strcpy(processor->name, "noisegate");
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
    noise_gate_processor_free(processor);
    return NULL;
  }

  double srate = (double)sample_rate;
  processor->attack = exp(-1.0 / srate / params->attack);
  processor->release = exp(-1.0 / srate / params->release);
  processor->threshold = params->threshold;
  processor->factor = double_from_db(-params->attenuation);
  processor->prev_loudness = 0.0;

  return processor;
}

void noise_gate_processor_free(noise_gate_processor_t* processor) {
  if (!processor) return;
  free(processor->monitor_channels);
  free(processor->process_channels);
  free(processor->scratch);
  free(processor);
}

void noise_gate_processor_process(noise_gate_processor_t* processor,
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

  // Step 3: Gate Threshold Logic
  // For each sample, compare the smoothed envelope level against the threshold.
  for (size_t i = 0; i < count; i++) {
    if (processor->scratch[i] < processor->threshold) {
      // Below threshold: gate closed, apply the pre-calculated linear
      // attenuation factor.
      processor->scratch[i] = processor->factor;
    } else {
      // Above or equal to threshold: gate open, pass signal through (unity
      // gain).
      processor->scratch[i] = 1.0;
    }
  }

  // Step 4: Apply gating gain curve to all processed channels
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
  }
}

void noise_gate_processor_update_parameters(noise_gate_processor_t* processor,
                                            const processor_config_t* config,
                                            int sample_rate) {
  if (!processor || !config || sample_rate <= 0) return;
  if (config->type != PROCESSOR_TYPE_NOISE_GATE) return;
  const noise_gate_parameters_t* params = &config->parameters.noise_gate;

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
  processor->factor = double_from_db(-params->attenuation);
}

void noise_gate_processor_transfer_state(noise_gate_processor_t* dest,
                                         const noise_gate_processor_t* src) {
  if (!dest || !src) return;
  dest->prev_loudness = src->prev_loudness;
}
