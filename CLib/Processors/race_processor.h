#ifndef CLIB_PROCESSORS_RACE_PROCESSOR_H
#define CLIB_PROCESSORS_RACE_PROCESSOR_H

/**
 * @file race_processor.h
 * @brief RACE (Recursive Attenuator and Cross-talk Cancellation) processor module.
 *
 * This module implements the RACE algorithm for binaural/stereo acoustic cross-talk cancellation.
 *
 * RACE Cross-Talk Cancellation Math Explanation:
 * 1. Acoustic Cross-Talk Problem:
 *    - When listening to stereo loudspeakers, sound from the left speaker reaches the right ear
 *      (contralateral path) after a short time delay and acoustic attenuation, and vice versa.
 *    - This cross-talk degrades spatial imaging and binaural cues.
 * 2. Recursive Cross-Talk Cancellation:
 *    - To cancel the contralateral signal at the listener's ears, a delayed and attenuated inverted
 *      version of the contralateral channel is added to the ipsilateral channel.
 *    - Because adding a cancellation signal creates a secondary cross-talk path (which in turn must
 *      be cancelled), a recursive feedback loop is employed.
 * 3. Processing Algorithm per Sample `i`:
 *    - Let `val_A` and `val_B` be the input samples for channels A and B at time index `i`.
 *    - Let `feedback_A` and `feedback_B` be the recursive cancellation signals from the previous step.
 *    - Step 1: Add contralateral feedback cancellation signals:
 *      added_A = val_A + feedback_B
 *      added_B = val_B + feedback_A
 *    - Step 2: Pass the combined signals through delay filters (`delay_A`, `delay_B`) representing the
 *      interaural time difference (ITD):
 *      delayed_A = delay_filter_process_single(delay_A, added_A)
 *      delayed_B = delay_filter_process_single(delay_B, added_B)
 *    - Step 3: Pass the delayed signals through gain filters (`gain`) representing acoustic attenuation
 *      and phase inversion (negative gain):
 *      feedback_A = gain_filter_process_single(gain, delayed_A)
 *      feedback_B = gain_filter_process_single(gain, delayed_B)
 *    - Step 4: Output the cancelled samples:
 *      out_A[i] = added_A
 *      out_B[i] = added_B
 * 4. Delay Unit Conversion & Subsample Accuracy:
 *    - Supports delay units in microseconds (us), milliseconds (ms), millimeters (mm @ 343 m/s), and samples.
 *    - Compensates for 1 sample period processing latency: compensated_delay = max(delay - sample_period, 0.0).
 * 5. ZERO-ALLOCATION GUARANTEE: Real-time processing (`race_processor_process`) performs no
 *    memory allocations. All delay lines and state variables are pre-allocated during initialization.
 */

#include <stddef.h>
#include <stdbool.h>
#include "Audio/audio_chunk.h"
#include "Audio/prc_fmt.h"
#include "Config/processor_config_types.h"
#include "Filters/delay.h"
#include "Filters/gain.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RACE cross-talk cancellation processor state structure.
 */
typedef struct {
    char name[64];           ///< Unique name of the RACE processor instance.
    int channel_a;           ///< Index of primary channel A (e.g., Left).
    int channel_b;           ///< Index of primary channel B (e.g., Right).
    delay_filter_t* delay_a; ///< Contralateral delay line filter for channel A path.
    delay_filter_t* delay_b; ///< Contralateral delay line filter for channel B path.
    gain_filter_t* gain;     ///< Attenuation and phase-inversion gain filter.
    prc_fmt_t feedback_a;    ///< Recursive feedback sample from channel A delay/gain path.
    prc_fmt_t feedback_b;    ///< Recursive feedback sample from channel B delay/gain path.
} race_processor_t;

/**
 * @brief Creates a new RACE cross-talk cancellation processor.
 *
 * @param name Unique name for this RACE instance.
 * @param params RACE configuration parameters (channel indices, delay, attenuation, delay unit, subsample flag).
 * @param sample_rate Audio sample rate in Hz.
 * @return Pointer to newly allocated race_processor_t, or NULL on failure.
 */
race_processor_t* race_processor_create(const char* name, const race_parameters_t* params, int sample_rate);

/**
 * @brief Frees all resources associated with the RACE processor.
 *
 * @param processor Pointer to RACE processor to free.
 */
void race_processor_free(race_processor_t* processor);

/**
 * @brief Applies RACE cross-talk cancellation to audio chunk in place.
 *
 * Evaluates sample-by-sample recursive feedback loop across channel A and channel B.
 *
 * @param processor Pointer to RACE processor.
 * @param chunk Audio chunk to process in place.
 */
void race_processor_process(race_processor_t* processor, audio_chunk_t* chunk);

/**
 * @brief Updates RACE parameters dynamically.
 *
 * Re-computes compensated delay time and attenuation gain without reallocating delay buffers.
 *
 * @param processor Pointer to RACE processor.
 * @param config New processor configuration.
 * @param sample_rate Audio sample rate in Hz.
 */
void race_processor_update_parameters(race_processor_t* processor, const processor_config_t* config, int sample_rate);


#ifdef __cplusplus
}
#endif

#endif // CLIB_PROCESSORS_RACE_PROCESSOR_H
