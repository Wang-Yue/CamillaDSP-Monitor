/**
 * @file race_processor.c
 * @brief Implementation of the RACE cross-talk cancellation processor.
 *
 * Implementation details:
 * - Delay Unit Conversion: Converts user-configured delay units into
 * milliseconds/samples.
 * - Latency Compensation: Subtracted 1 sample period (e.g. 1000.0/sample_rate
 * in ms) from target delay to compensate for 1-sample processing feedback
 * latency: compensated_delay = max(delay - sample_period, 0.0)
 * - Gain Setup: Configured with negative dB attenuation and inverted phase
 * (`inverted = true`).
 * - Real-time processing (`race_processor_process`):
 *   Sample-by-sample feedback loop evaluating:
 *   1. added_A = val_A + feedback_B; added_B = val_B + feedback_A
 *   2. Pass added_A/B through interaural delay lines (`delay_A`, `delay_B`).
 *   3. Pass delayed samples through attenuation & phase inversion gain filter.
 *   4. Store new feedback samples and update output waveforms in place.
 */

#include "race_processor.h"
#include "Logging/app_logger.h"

struct race_processor {
  char name[64];  ///< Unique name of the RACE processor instance.
  int channel_a;  ///< Index of primary channel A (e.g., Left).
  int channel_b;  ///< Index of primary channel B (e.g., Right).
  delay_filter_t*
      delay_a;  ///< Contralateral delay line filter for channel A path.
  delay_filter_t*
      delay_b;          ///< Contralateral delay line filter for channel B path.
  gain_filter_t* gain;  ///< Attenuation and phase-inversion gain filter.
  double feedback_a;    ///< Recursive feedback sample from channel A delay/gain
                        ///< path.
  double feedback_b;    ///< Recursive feedback sample from channel B delay/gain
                        ///< path.
  bool channel_warning_logged; ///< Track if we already logged a channel mismatch warning.
};

const char* race_processor_get_name(const race_processor_t* processor) {
  return processor ? processor->name : "";
}

#include <math.h>
#include <stdlib.h>
#include <string.h>

race_processor_t* race_processor_create(const char* name,
                                        const race_parameters_t* params,
                                        int sample_rate,
                                        config_error_t* err) {
  if (!params) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER, "RACE params is NULL");
    return NULL;
  }
  if (sample_rate <= 0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER, "RACE sample_rate must be positive");
    return NULL;
  }

  race_processor_t* processor =
      (race_processor_t*)calloc(1, sizeof(race_processor_t));
  if (!processor) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate RACE processor wrapper");
    return NULL;
  }

  if (name) {
    strncpy(processor->name, name, sizeof(processor->name) - 1);
    processor->name[sizeof(processor->name) - 1] = '\0';
  } else {
    strcpy(processor->name, "race");
  }

  processor->channel_a = params->channel_a < params->channel_b
                             ? params->channel_a
                             : params->channel_b;
  processor->channel_b = params->channel_a > params->channel_b
                             ? params->channel_a
                             : params->channel_b;

  delay_unit_t unit =
      params->has_delay_unit ? params->delay_unit : DELAY_UNIT_MS;

  /* Calculate the duration of one sample in the requested delay units.
     This is used to compensate for the implicit 1-sample delay introduced
     by the recursive feedback structure in the process loop.
     For MM (millimeters), we assume speed of sound is 343 m/s. */
  double sample_period = 1.0;
  switch (unit) {
    case DELAY_UNIT_US:
      sample_period = 1000000.0 / (double)sample_rate;
      break;
    case DELAY_UNIT_MS:
      sample_period = 1000.0 / (double)sample_rate;
      break;
    case DELAY_UNIT_MM:
      sample_period = 343.0 * 1000.0 / (double)sample_rate;
      break;
    case DELAY_UNIT_SAMPLES:
      sample_period = 1.0;
      break;
    default:
      sample_period = 1000.0 / (double)sample_rate;
      break;
  }

  /* Compensate for the 1-sample pipeline delay in the feedback path.
     Since the feedback signal is applied in the next sample period, we must
     subtract one sample period from the target delay line length.
     Clamp to 0.0 if target delay is smaller than one sample. */
  double comp_delay = params->delay - sample_period;
  if (comp_delay < 0.0) comp_delay = 0.0;

  delay_parameters_t dparams = {0};
  dparams.delay = comp_delay;
  dparams.unit = unit;
  dparams.subsample =
      params->has_subsample_delay ? params->subsample_delay : false;

  processor->delay_a =
      delay_filter_create("race-DelayA", &dparams, sample_rate, err);
  processor->delay_b =
      delay_filter_create("race-DelayB", &dparams, sample_rate, err);

  gain_parameters_t gparams = {0};
  gparams.gain = -params->attenuation;
  gparams.has_gain = true;
  gparams.scale = GAIN_SCALE_DB;
  gparams.inverted = true;
  gparams.mute = false;

  processor->gain = gain_filter_create("race-Gain", &gparams);
  processor->feedback_a = 0.0;
  processor->feedback_b = 0.0;

  if (!processor->delay_a || !processor->delay_b || !processor->gain) {
    if (err && err->type == CONFIG_ERR_NONE) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Failed to initialize sub-filters for RACE processor '%s'",
                       processor->name);
    }
    race_processor_free(processor);
    return NULL;
  }

  return processor;
}

void race_processor_free(race_processor_t* processor) {
  if (!processor) return;
  if (processor->delay_a) delay_filter_free(processor->delay_a);
  if (processor->delay_b) delay_filter_free(processor->delay_b);
  if (processor->gain) gain_filter_free(processor->gain);
  free(processor);
}

void race_processor_process(race_processor_t* processor, audio_chunk_t* chunk) {
  if (!processor || !chunk) return;
  size_t count = audio_chunk_get_valid_frames(chunk);
  if (count == 0 || !processor->delay_a || !processor->delay_b ||
      !processor->gain)
    return;

  double* base_a = audio_chunk_get_channel(chunk, processor->channel_a);
  double* base_b = audio_chunk_get_channel(chunk, processor->channel_b);
  if (!base_a || !base_b) {
    if (!processor->channel_warning_logged) {
      logger_t logger = logger_create("race_processor");
      logger_error(&logger, "RACE channel indices (%d, %d) out of bounds for chunk channels (%d)",
                   log_arg_int((int64_t)processor->channel_a),
                   log_arg_int((int64_t)processor->channel_b),
                   log_arg_int((int64_t)audio_chunk_get_channels(chunk)),
                   log_arg_none());
      processor->channel_warning_logged = true;
    }
    return;
  }

  // Evaluate sample-by-sample recursive cross-talk cancellation loop
  for (size_t i = 0; i < count; i++) {
    double val_a = base_a[i];
    double val_b = base_b[i];

    // Step 1: Add contralateral cancellation feedback signal from previous
    // sample step
    double added_a = val_a + processor->feedback_b;
    double added_b = val_b + processor->feedback_a;

    // Step 2: Pass combined signals through delay filters representing
    // interaural time difference (ITD)
    processor->feedback_a =
        delay_filter_process_single(processor->delay_a, added_a);
    processor->feedback_b =
        delay_filter_process_single(processor->delay_b, added_b);

    // Step 3: Pass delayed signals through gain filter representing acoustic
    // attenuation and phase inversion
    processor->feedback_a =
        gain_filter_process_single(processor->gain, processor->feedback_a);
    processor->feedback_b =
        gain_filter_process_single(processor->gain, processor->feedback_b);

    // Step 4: Output cross-talk cancelled samples in place
    base_a[i] = added_a;
    base_b[i] = added_b;
  }
}

void race_processor_transfer_state(race_processor_t* dest,
                                   const race_processor_t* src) {
  if (!dest || !src) return;
  dest->feedback_a = src->feedback_a;
  dest->feedback_b = src->feedback_b;
}
