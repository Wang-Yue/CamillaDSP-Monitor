#ifndef CLIB_PROCESSORS_COMPRESSOR_PROCESSOR_H
#define CLIB_PROCESSORS_COMPRESSOR_PROCESSOR_H

/**
 * @file compressor_processor.h
 * @brief Dynamic range compressor processor module.
 *
 * This module implements a multi-channel dynamic range compressor with optional
 * soft/hard clipping limiter.
 *
 * Compressor Envelope Detection & Gain Reduction Curves Explanation:
 * 1. Channel Monitoring & Summing:
 *    - The compressor monitors a specified subset of input channels (or all
 * channels if unspecified).
 *    - The monitored channels are summed together into a scratch buffer to
 * evaluate overall signal level.
 * 2. Envelope Detection (Loudness Estimation):
 *    - For each sample, the instantaneous loudness in dB is calculated as:
 *      val_db = 20.0 * log10(abs(sample) + 1e-9).
 *    - A first-order IIR exponential smoothing filter tracks the envelope:
 *      - Attack coefficient: attack = exp(-1.0 / (sample_rate * attack_time)).
 *      - Release coefficient: release = exp(-1.0 / (sample_rate *
 * release_time)).
 *      - If val_db >= prev_loudness (signal level rising):
 *        loudness = attack * prev_loudness + (1.0 - attack) * val_db.
 *      - If val_db < prev_loudness (signal level falling):
 *        loudness = release * prev_loudness + (1.0 - release) * val_db.
 * 3. Gain Reduction Curve:
 *    - If the estimated loudness exceeds the threshold (loudness > threshold):
 *      gain_reduction_db = -(loudness - threshold) * (factor - 1.0) / factor.
 *    - Otherwise (loudness <= threshold):
 *      gain_reduction_db = 0.0 dB (unity gain).
 *    - The final linear gain multiplier applied to processing channels is:
 *      lin_gain = 10^((gain_reduction_db + makeup_gain_db) / 20).
 * 4. Limiting:
 *    - An optional post-compression limiter (`limiter_filter_t`) prevents
 * clipping on processed channels.
 * 5. ZERO-ALLOCATION GUARANTEE: Real-time processing
 * (`compressor_processor_process`) performs no memory allocations. All scratch
 * buffers are pre-allocated during initialization.
 */

#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Audio/double_helpers.h"
#include "Config/processor_config_types.h"
#include "Filters/limiter.h"

/**
 * @brief Dynamic range compressor processor state structure.
 */
typedef struct compressor_processor compressor_processor_t;

/**
 * @brief Get the name of the compressor processor.
 *
 * @param[in] processor Pointer to compressor processor.
 * @return The name of the processor.
 */
const char* compressor_processor_get_name(
    const compressor_processor_t* processor);

/**
 * @brief Creates a new dynamic range compressor processor.
 *
 * @param name Unique name for this compressor instance.
 * @param params Compressor configuration parameters (attack, release,
 * threshold, factor, etc.).
 * @param sample_rate Audio sample rate in Hz.
 * @param chunk_size Maximum number of frames per processing chunk.
 * @return Pointer to newly allocated compressor_processor_t, or NULL on
 * failure.
 */
compressor_processor_t* compressor_processor_create(
    const char* name, const compressor_parameters_t* params, int sample_rate,
    size_t chunk_size);

/**
 * @brief Frees all resources associated with the compressor processor.
 *
 * @param processor Pointer to compressor processor to free.
 */
void compressor_processor_free(compressor_processor_t* processor);

/**
 * @brief Applies dynamic range compression to audio chunk in place.
 *
 * Evaluates monitored channels, computes envelope loudness and gain reduction
 * curve, applies linear gain to processed channels, and runs optional limiter.
 *
 * @param processor Pointer to compressor processor.
 * @param chunk Audio chunk to process in place.
 */
void compressor_processor_process(compressor_processor_t* processor,
                                  audio_chunk_t* chunk);

/**
 * @brief Transfers running envelope loudness state from src to dest.
 *
 * @param dest The destination compressor processor instance.
 * @param src The source compressor processor instance.
 */
void compressor_processor_transfer_state(compressor_processor_t* dest,
                                         const compressor_processor_t* src);

#endif  // CLIB_PROCESSORS_COMPRESSOR_PROCESSOR_H
