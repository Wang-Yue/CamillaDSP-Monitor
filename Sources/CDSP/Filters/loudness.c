#include "loudness.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// RME ADI-2 DAC Loudness Curves
// https://www.rme-audio.de/downloads/adi2dac_e.pdf

static void recompute_shelves(loudness_filter_t* filter, double volume) {
  double ref = filter->params.has_reference_level
                   ? filter->params.reference_level
                   : -25.0;
  double diff = (ref - volume) / 20.0;
  double boost_factor = diff < 0.0 ? 0.0 : (diff > 1.0 ? 1.0 : diff);

  filter->is_processing_active = boost_factor > 0.001;

  double low_boost =
      (filter->params.has_low_boost ? filter->params.low_boost : 10.0) *
      boost_factor;
  double high_boost =
      (filter->params.has_high_boost ? filter->params.high_boost : 10.0) *
      boost_factor;

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
    processing_parameters_t* proc_params) {
  loudness_filter_t* filter =
      (loudness_filter_t*)malloc(sizeof(loudness_filter_t));
  if (!filter) return NULL;
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

  filter->low_shelf_filter = biquad_filter_create("loudness_ls", NULL);
  filter->high_shelf_filter = biquad_filter_create("loudness_hs", NULL);

  return filter;
}

void loudness_filter_process(loudness_filter_t* filter,
                             mutable_waveform_t waveform, size_t count) {
  if (!filter || !waveform || count == 0) return;
  if (!filter->processing_parameters) return;

  double current_vol = processing_parameters_get_current_volume_for_fader(
      filter->processing_parameters, filter->params.fader);
  // Recompute coefficients if volume changed significantly
  if (fabs(current_vol - filter->last_volume) > 0.01 ||
      !filter->is_processing_active) {
    filter->last_volume = current_vol;
    recompute_shelves(filter, current_vol);
  }

  if (!filter->is_processing_active) return;

  // Apply filters in order
  biquad_filter_process(filter->high_shelf_filter, waveform, count);
  biquad_filter_process(filter->low_shelf_filter, waveform, count);

  // Apply midband attenuation if enabled
  if (filter->params.attenuate_mid &&
      fabs(filter->midband_attenuation_db) > 0.001) {
    double factor = double_from_db(filter->midband_attenuation_db);
    dsp_ops_scalar_multiply(waveform, factor, count);
  }
}

void loudness_filter_update_parameters(loudness_filter_t* filter,
                                       const filter_config_t* config,
                                       int sample_rate) {
  (void)sample_rate;
  if (!filter || !config) return;
  if (config->type != FILTER_TYPE_LOUDNESS) return;
  filter->params = config->parameters.loudness;
  filter->is_processing_active = false;
}

void loudness_filter_free(loudness_filter_t* filter) {
  if (!filter) return;
  if (filter->low_shelf_filter) biquad_filter_free(filter->low_shelf_filter);
  if (filter->high_shelf_filter) biquad_filter_free(filter->high_shelf_filter);
  free(filter);
}
