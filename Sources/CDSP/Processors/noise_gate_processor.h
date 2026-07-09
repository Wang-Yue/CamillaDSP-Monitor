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
typedef struct noise_gate_processor noise_gate_processor_t;

/**
 * @brief Gets the name of the noise gate processor.
 *
 * @param processor Pointer to the noise gate processor.
 * @return The unique name of the processor instance.
 */
const char* noise_gate_processor_get_name(
    const noise_gate_processor_t* processor);

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
 * @brief Transfers running envelope loudness state from src to dest.
 *
 * @param dest The destination noise gate processor instance.
 * @param src The source noise gate processor instance.
 */
void noise_gate_processor_transfer_state(noise_gate_processor_t* dest,
                                         const noise_gate_processor_t* src);

#endif  // CLIB_PROCESSORS_NOISE_GATE_PROCESSOR_H
