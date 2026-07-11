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
#include "Logging/app_logger.h"

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
  bool channel_warning_logged; ///< Track if we already logged a channel mismatch warning.
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
    noise_gate_processor_free(processor);
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
      logger_t logger = logger_create("noise_gate_processor");
      logger_error(&logger, "Noise Gate channel indices out of bounds for chunk channels (%d)",
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
  audio_chunk_apply_gain(chunk, processor->process_channels,
                         processor->process_channels_count, processor->scratch,
                         count);
}

void noise_gate_processor_transfer_state(noise_gate_processor_t* dest,
                                         const noise_gate_processor_t* src) {
  if (!dest || !src) return;
  dest->prev_loudness = src->prev_loudness;
}
