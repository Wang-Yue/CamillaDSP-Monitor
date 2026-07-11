#ifndef CLIB_PROCESSORS_RACE_PROCESSOR_H
#define CLIB_PROCESSORS_RACE_PROCESSOR_H

/**
 * @file race_processor.h
 * @brief RACE (Recursive Attenuator and Cross-talk Cancellation) processor
 * module.
 *
 * This module implements the RACE algorithm for binaural/stereo acoustic
 * cross-talk cancellation.
 *
 * RACE Cross-Talk Cancellation Math Explanation:
 * 1. Acoustic Cross-Talk Problem:
 *    - When listening to stereo loudspeakers, sound from the left speaker
 * reaches the right ear (contralateral path) after a short time delay and
 * acoustic attenuation, and vice versa.
 *    - This cross-talk degrades spatial imaging and binaural cues.
 * 2. Recursive Cross-Talk Cancellation:
 *    - To cancel the contralateral signal at the listener's ears, a delayed and
 * attenuated inverted version of the contralateral channel is added to the
 * ipsilateral channel.
 *    - Because adding a cancellation signal creates a secondary cross-talk path
 * (which in turn must be cancelled), a recursive feedback loop is employed.
 * 3. Processing Algorithm per Sample `i`:
 *    - Let `val_A` and `val_B` be the input samples for channels A and B at
 * time index `i`.
 *    - Let `feedback_A` and `feedback_B` be the recursive cancellation signals
 * from the previous step.
 *    - Step 1: Add contralateral feedback cancellation signals:
 *      added_A = val_A + feedback_B
 *      added_B = val_B + feedback_A
 *    - Step 2: Pass the combined signals through delay filters (`delay_A`,
 * `delay_B`) representing the interaural time difference (ITD): delayed_A =
 * delay_filter_process_single(delay_A, added_A) delayed_B =
 * delay_filter_process_single(delay_B, added_B)
 *    - Step 3: Pass the delayed signals through gain filters (`gain`)
 * representing acoustic attenuation and phase inversion (negative gain):
 *      feedback_A = gain_filter_process_single(gain, delayed_A)
 *      feedback_B = gain_filter_process_single(gain, delayed_B)
 *    - Step 4: Output the cancelled samples:
 *      out_A[i] = added_A
 *      out_B[i] = added_B
 * 4. Delay Unit Conversion & Subsample Accuracy:
 *    - Supports delay units in microseconds (us), milliseconds (ms),
 * millimeters (mm @ 343 m/s), and samples.
 *    - Compensates for 1 sample period processing latency: compensated_delay =
 * max(delay - sample_period, 0.0).
 * 5. ZERO-ALLOCATION GUARANTEE: Real-time processing (`race_processor_process`)
 * performs no memory allocations. All delay lines and state variables are
 * pre-allocated during initialization.
 */

#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Audio/double_helpers.h"
#include "Config/processor_config_types.h"
#include "Filters/delay.h"
#include "Filters/gain.h"

/**
 * @brief RACE cross-talk cancellation processor state structure.
 */
typedef struct race_processor race_processor_t;

/**
 * @brief Gets the name of the RACE processor.
 *
 * @param processor Pointer to the RACE processor.
 * @return The unique name of the processor instance.
 */
const char* race_processor_get_name(const race_processor_t* processor);

/**
 * @brief Creates a new RACE cross-talk cancellation processor.
 *
 * @param name Unique name for this RACE instance.
 * @param params RACE configuration parameters (channel indices, delay,
 * attenuation, delay unit, subsample flag).
 * @param sample_rate Audio sample rate in Hz.
 * @param err Pointer to a config error struct to populate on failure.
 * @return Pointer to newly allocated race_processor_t, or NULL on failure.
 */
race_processor_t* race_processor_create(const char* name,
                                        const race_parameters_t* params,
                                        int sample_rate,
                                        config_error_t* err);

/**
 * @brief Frees all resources associated with the RACE processor.
 *
 * @param processor Pointer to RACE processor to free.
 */
void race_processor_free(race_processor_t* processor);

/**
 * @brief Applies RACE cross-talk cancellation to audio chunk in place.
 *
 * Evaluates sample-by-sample recursive feedback loop across channel A and
 * channel B.
 *
 * @param processor Pointer to RACE processor.
 * @param chunk Audio chunk to process in place.
 */
void race_processor_process(race_processor_t* processor, audio_chunk_t* chunk);

/**
 * @brief Transfers recursive feedback loop registers from src to dest.
 *
 * @param dest The destination RACE processor instance.
 * @param src The source RACE processor instance.
 */
void race_processor_transfer_state(race_processor_t* dest,
                                   const race_processor_t* src);

#endif  // CLIB_PROCESSORS_RACE_PROCESSOR_H
