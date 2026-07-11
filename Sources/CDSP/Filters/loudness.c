#include "loudness.h"

#include "biquad.h"

struct loudness_filter {
  char name[64];
  int sample_rate;
  loudness_parameters_t params;
  biquad_filter_t* low_shelf_filter;
  biquad_filter_t* high_shelf_filter;
  double last_volume;
  bool is_processing_active;
  double midband_attenuation_db;
  processing_parameters_t* processing_parameters;
};

#include <math.h>
#include <stdlib.h>
#include <string.h>

// RME ADI-2 DAC Loudness Curves
// https://www.rme-audio.de/downloads/adi2dac_e.pdf

/**
 * @brief Recomputes the low-shelf and high-shelf filter coefficients based on
 * the current volume.
 *
 * Implements the RME ADI-2 DAC Loudness Curve logic:
 * - A reference volume level is compared against the current volume.
 * - The difference determines a boost factor between 0.0 (no boost) and 1.0
 * (maximum configured boost).
 * - Low and high frequencies are boosted proportionally.
 * - Optionally, midband attenuation can be used instead of frequency boosts to
 * prevent clipping.
 *
 * @param filter The loudness filter instance.
 * @param volume The current volume in dB.
 */
static void recompute_shelves(loudness_filter_t* filter, double volume) {
  double ref = filter->params.has_reference_level
                   ? filter->params.reference_level
                   : -25.0;

  // Calculate boost factor. 1.0 boost factor corresponds to 20 dB or more
  // attenuation below the reference level.
  double diff = (ref - volume) / 20.0;
  double boost_factor = diff < 0.0 ? 0.0 : (diff > 1.0 ? 1.0 : diff);

  filter->is_processing_active = boost_factor > 0.001;

  // Calculate target low and high boosts.
  double low_boost =
      (filter->params.has_low_boost ? filter->params.low_boost : 10.0) *
      boost_factor;
  double high_boost =
      (filter->params.has_high_boost ? filter->params.high_boost : 10.0) *
      boost_factor;

  // If attenuate_mid is enabled, we lower the overall gain (attenuate midband)
  // by the maximum boost instead of boosting the shelves. This keeps the peak
  // gain at 0 dB, preventing digital clipping.
  if (filter->params.attenuate_mid) {
    double max_boost = low_boost > high_boost ? low_boost : high_boost;
    filter->midband_attenuation_db = -max_boost;
  } else {
    filter->midband_attenuation_db = 0.0;
  }

  // Low shelf at 70 Hz, 12 dB/oct slope
  // Update coefficients in-place to preserve biquad delay-line state (no
  // clicks)
  filter_config_t lp_cfg;
  memset(&lp_cfg, 0, sizeof(lp_cfg));
  lp_cfg.type = FILTER_TYPE_BIQUAD;
  lp_cfg.parameters.biquad.type = BIQUAD_TYPE_LOWSHELF;
  lp_cfg.parameters.biquad.freq = 70.0;
  lp_cfg.parameters.biquad.gain = low_boost;
  lp_cfg.parameters.biquad.slope = 12.0;
  lp_cfg.parameters.biquad.steepness_type = STEEPNESS_TYPE_SLOPE;
  biquad_filter_update_parameters(filter->low_shelf_filter, &lp_cfg,
                                  filter->sample_rate);

  // High shelf at 3500 Hz, 12 dB/oct slope
  filter_config_t hp_cfg;
  memset(&hp_cfg, 0, sizeof(hp_cfg));
  hp_cfg.type = FILTER_TYPE_BIQUAD;
  hp_cfg.parameters.biquad.type = BIQUAD_TYPE_HIGHSHELF;
  hp_cfg.parameters.biquad.freq = 3500.0;
  hp_cfg.parameters.biquad.gain = high_boost;
  hp_cfg.parameters.biquad.slope = 12.0;
  hp_cfg.parameters.biquad.steepness_type = STEEPNESS_TYPE_SLOPE;
  biquad_filter_update_parameters(filter->high_shelf_filter, &hp_cfg,
                                  filter->sample_rate);
}

loudness_filter_t* loudness_filter_create(
    const char* name, const loudness_parameters_t* params, int sample_rate,
    processing_parameters_t* proc_params, config_error_t* err) {
  loudness_filter_t* filter =
      (loudness_filter_t*)calloc(1, sizeof(loudness_filter_t));
  if (!filter) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate loudness filter wrapper");
    return NULL;
  }
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "loudness");
  }
  filter->sample_rate = sample_rate;
  if (params) {
    filter->params = *params;
  } else {
    memset(&filter->params, 0, sizeof(loudness_parameters_t));
  }
  filter->processing_parameters = proc_params;
  filter->last_volume = 0.0;
  filter->is_processing_active = false;
  filter->midband_attenuation_db = 0.0;

  filter->low_shelf_filter = biquad_filter_create("loudness_ls", NULL, err);
  filter->high_shelf_filter = biquad_filter_create("loudness_hs", NULL, err);
  if (!filter->low_shelf_filter || !filter->high_shelf_filter) {
    loudness_filter_free(filter);
    return NULL;
  }

  return filter;
}

void loudness_filter_process(loudness_filter_t* filter,
                             mutable_waveform_t waveform, size_t count) {
  if (!filter || !waveform || count == 0) return;
  if (!filter->processing_parameters) return;

  double current_vol = processing_parameters_get_current_volume_for_fader(
      filter->processing_parameters, filter->params.fader);

  // Recompute filter coefficients only if the volume has changed significantly.
  // This avoids expensive filter re-calculation (and potential audio glitches)
  // for tiny volume fluctuations.
  if (fabs(current_vol - filter->last_volume) > 0.01 ||
      !filter->is_processing_active) {
    filter->last_volume = current_vol;
    recompute_shelves(filter, current_vol);
  }

  // If the volume is high enough that loudness compensation is not needed,
  // bypass processing.
  if (!filter->is_processing_active) return;

  // Apply high-shelf and low-shelf filters.
  biquad_filter_process(filter->high_shelf_filter, waveform, count);
  biquad_filter_process(filter->low_shelf_filter, waveform, count);

  // Apply midband attenuation if enabled to simulate bass/treble boost
  // without exceeding 0 dBFS peak gain.
  if (filter->params.attenuate_mid &&
      fabs(filter->midband_attenuation_db) > 0.001) {
    double factor = double_from_db(filter->midband_attenuation_db);
    dsp_ops_scalar_multiply(waveform, factor, count);
  }
}

void loudness_filter_transfer_state(loudness_filter_t* dest,
                                    const loudness_filter_t* src) {
  if (!dest || !src) return;
  biquad_filter_transfer_state(dest->low_shelf_filter, src->low_shelf_filter);
  biquad_filter_transfer_state(dest->high_shelf_filter, src->high_shelf_filter);
  dest->last_volume = src->last_volume;
}

void loudness_filter_free(loudness_filter_t* filter) {
  if (!filter) return;
  if (filter->low_shelf_filter) biquad_filter_free(filter->low_shelf_filter);
  if (filter->high_shelf_filter) biquad_filter_free(filter->high_shelf_filter);
  free(filter);
}
