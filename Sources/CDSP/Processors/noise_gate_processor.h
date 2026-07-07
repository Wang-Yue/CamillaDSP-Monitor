#ifndef CLIB_PROCESSORS_NOISE_GATE_PROCESSOR_H
#define CLIB_PROCESSORS_NOISE_GATE_PROCESSOR_H

/**
 * @file noise_gate_processor.h
 * @brief Multi-channel noise gate processor module.
 *
 * This module implements a noise gate that attenuates audio signals below a
 * specified loudness threshold.
 *
 * Noise Gate Thresholds & Envelope Detection Explanation:
 * 1. Channel Monitoring & Summing:
 *    - Monitored channels are summed together into a scratch buffer to evaluate
 * overall signal loudness.
 * 2. Envelope Detection (Loudness Estimation):
 *    - Instantaneous loudness in dB: val_db = 20.0 * log10(abs(sample) + 1e-9).
 *    - Exponential smoothing filter tracks envelope with first-order IIR
 * attack/release constants: attack = exp(-1.0 / (sample_rate * attack_time))
 *      release = exp(-1.0 / (sample_rate * release_time))
 * 3. Gate Threshold Logic:
 *    - If estimated loudness falls below the threshold (loudness < threshold):
 *      gain = factor = 10^(-attenuation_db / 20).
 *    - Otherwise (loudness >= threshold):
 *      gain = 1.0 (unity gain, gate open).
 * 4. Gain Application:
 *    - Linear gain multiplier is applied across all processed channels using
 * Apple Accelerate (vDSP) or scalar fallback.
 * 5. ZERO-ALLOCATION GUARANTEE: Real-time processing
 * (`noise_gate_processor_process`) performs no memory allocations. All scratch
 * buffers are pre-allocated during initialization.
 */

#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Audio/double_helpers.h"
#include "Config/processor_config_types.h"

/**
 * @brief Noise gate processor state structure.
 */
typedef struct {
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
} noise_gate_processor_t;

/**
 * @brief Creates a new noise gate processor.
 *
 * @param name Unique name for this noise gate instance.
 * @param params Noise gate parameters (attack, release, threshold, attenuation,
 * etc.).
 * @param sample_rate Audio sample rate in Hz.
 * @param chunk_size Maximum number of frames per processing chunk.
 * @return Pointer to newly allocated noise_gate_processor_t, or NULL on
 * failure.
 */
noise_gate_processor_t* noise_gate_processor_create(
    const char* name, const noise_gate_parameters_t* params, int sample_rate,
    size_t chunk_size);

/**
 * @brief Frees all resources associated with the noise gate processor.
 *
 * @param processor Pointer to noise gate processor to free.
 */
void noise_gate_processor_free(noise_gate_processor_t* processor);

/**
 * @brief Applies noise gating to audio chunk in place.
 *
 * Evaluates monitored channels, computes envelope loudness and gate threshold
 * gain, and applies linear attenuation to processed channels when gate is
 * closed.
 *
 * @param processor Pointer to noise gate processor.
 * @param chunk Audio chunk to process in place.
 */
void noise_gate_processor_process(noise_gate_processor_t* processor,
                                  audio_chunk_t* chunk);

/**
 * @brief Updates noise gate parameters dynamically.
 *
 * Re-computes attack/release smoothing coefficients and attenuation factor.
 *
 * @param processor Pointer to noise gate processor.
 * @param config New processor configuration.
 * @param sample_rate Audio sample rate in Hz.
 */
void noise_gate_processor_update_parameters(noise_gate_processor_t* processor,
                                            const processor_config_t* config,
                                            int sample_rate);

#endif  // CLIB_PROCESSORS_NOISE_GATE_PROCESSOR_H
