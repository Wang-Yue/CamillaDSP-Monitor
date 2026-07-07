#include "biquad_combo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// MARK: - Butterworth & Linkwitz-Riley helper calculations
size_t biquad_combo_butterworth_q(int order, double* out_q, size_t max_q) {
  if (order < 1 || !out_q || max_q == 0) return 0;
  size_t count = 0;
  for (int k = 0; k < order / 2; k++) {
    if (count >= max_q) break;
    double angle = M_PI / (double)order * ((double)k + 0.5);
    out_q[count++] = 1.0 / (2.0 * sin(angle));
  }
  if (order % 2 != 0 && count < max_q) {
    out_q[count++] = -1.0;
  }
  return count;
}

size_t biquad_combo_linkwitz_riley_q(int order, double* out_q, size_t max_q) {
  if (order % 2 != 0 || order < 2 || !out_q || max_q == 0) return 0;
  double bw_q[16];
  size_t bw_count = biquad_combo_butterworth_q(order / 2, bw_q, 16);
  if (order % 4 > 0 && bw_count > 0) {
    bw_count--;
  }
  size_t count = 0;
  for (size_t i = 0; i < bw_count; i++) {
    if (count < max_q) out_q[count++] = bw_q[i];
  }
  for (size_t i = 0; i < bw_count; i++) {
    if (count < max_q) out_q[count++] = bw_q[i];
  }
  if (order % 4 > 0 && count < max_q) {
    out_q[count++] = 0.5;
  }
  return count;
}

static biquad_filter_t* create_section(biquad_type_t type, double freq,
                                       double q, double gain, double slope,
                                       double bandwidth,
                                       steepness_type_t steepness_type,
                                       int sample_rate) {
  biquad_parameters_t bp;
  memset(&bp, 0, sizeof(bp));
  bp.type = type;
  bp.freq = freq;
  bp.q = q;
  bp.gain = gain;
  bp.slope = slope;
  bp.bandwidth = bandwidth;
  bp.steepness_type = steepness_type;
  biquad_coefficients_t coeffs;
  if (!biquad_coefficients_compute(&bp, sample_rate, &coeffs)) return NULL;
  return biquad_filter_create("combo_sec", &coeffs);
}

biquad_combo_filter_t* biquad_combo_filter_create(
    const char* name, const biquad_combo_parameters_t* params,
    int sample_rate) {
  if (!params) return NULL;
  biquad_combo_filter_t* filter =
      (biquad_combo_filter_t*)malloc(sizeof(biquad_combo_filter_t));
  if (!filter) return NULL;
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "biquad_combo");
  }

  biquad_filter_t* secs[32];
  size_t num = 0;

  switch (params->type) {
    case BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS:
    case BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS: {
      bool hp = (params->type == BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS);
      double q_vals[32];
      size_t nq = biquad_combo_butterworth_q(params->order, q_vals, 32);
      for (size_t i = 0; i < nq; i++) {
        biquad_type_t t;
        if (q_vals[i] < 0.0) {
          t = hp ? BIQUAD_TYPE_HIGHPASS_FO : BIQUAD_TYPE_LOWPASS_FO;
        } else {
          t = hp ? BIQUAD_TYPE_HIGHPASS : BIQUAD_TYPE_LOWPASS;
        }
        secs[num++] =
            create_section(t, params->freq, q_vals[i] > 0 ? q_vals[i] : 0.707,
                           0.0, 0.0, 0.0, STEEPNESS_TYPE_Q, sample_rate);
      }
      break;
    }
    case BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS:
    case BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS: {
      bool hp = (params->type == BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS);
      double q_vals[32];
      size_t nq = biquad_combo_linkwitz_riley_q(params->order, q_vals, 32);
      for (size_t i = 0; i < nq; i++) {
        biquad_type_t t = hp ? BIQUAD_TYPE_HIGHPASS : BIQUAD_TYPE_LOWPASS;
        secs[num++] = create_section(t, params->freq, q_vals[i], 0.0, 0.0, 0.0,
                                     STEEPNESS_TYPE_Q, sample_rate);
      }
      break;
    }
    // MARK: - Tilt EQ
    case BIQUAD_COMBO_TYPE_TILT: {
      double gain = params->has_gain ? params->gain : 0.0;
      secs[num++] =
          create_section(BIQUAD_TYPE_LOWSHELF, 110.0, 0.35, -gain / 2.0, 0.0,
                         0.0, STEEPNESS_TYPE_Q, sample_rate);
      secs[num++] =
          create_section(BIQUAD_TYPE_HIGHSHELF, 3500.0, 0.35, gain / 2.0, 0.0,
                         0.0, STEEPNESS_TYPE_Q, sample_rate);
      break;
    }
    // MARK: - Graphic EQ
    case BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER: {
      size_t nb = params->gains_count > 0 ? params->gains_count : 1;
      double fmin = params->freq_min > 0 ? params->freq_min : 20.0;
      double fmax = params->freq_max > 0 ? params->freq_max : 20000.0;
      double log_min = log2(fmin);
      double log_max = log2(fmax);
      double bw = (log_max - log_min) / (double)nb;
      for (size_t i = 0; i < nb; i++) {
        if (num >= 32) break;
        double g = params->gains[i];
        if (fabs(g) <= 0.001) continue;
        double log_freq = log_min + ((double)i + 0.5) * bw;
        double f = pow(2.0, log_freq);
        secs[num++] = create_section(BIQUAD_TYPE_PEAKING, f, 0.0, g, 0.0, bw,
                                     STEEPNESS_TYPE_BANDWIDTH, sample_rate);
      }
      break;
    }
    // MARK: - Five Point PEQ
    case BIQUAD_COMBO_TYPE_FIVE_POINT_PEQ: {
      // Low shelf
      secs[num++] = create_section(
          BIQUAD_TYPE_LOWSHELF, params->fls > 0 ? params->fls : 80.0,
          params->qls > 0 ? params->qls : 0.707, params->gls, 0.0, 0.0,
          STEEPNESS_TYPE_Q, sample_rate);
      // Mid bands
      secs[num++] = create_section(
          BIQUAD_TYPE_PEAKING, params->fp1 > 0 ? params->fp1 : 250.0,
          params->qp1 > 0 ? params->qp1 : 1.5, params->gp1, 0.0, 0.0,
          STEEPNESS_TYPE_Q, sample_rate);
      secs[num++] = create_section(
          BIQUAD_TYPE_PEAKING, params->fp2 > 0 ? params->fp2 : 1000.0,
          params->qp2 > 0 ? params->qp2 : 2.0, params->gp2, 0.0, 0.0,
          STEEPNESS_TYPE_Q, sample_rate);
      secs[num++] = create_section(
          BIQUAD_TYPE_PEAKING, params->fp3 > 0 ? params->fp3 : 4000.0,
          params->qp3 > 0 ? params->qp3 : 1.0, params->gp3, 0.0, 0.0,
          STEEPNESS_TYPE_Q, sample_rate);
      // High shelf
      secs[num++] = create_section(
          BIQUAD_TYPE_HIGHSHELF, params->fhs > 0 ? params->fhs : 12000.0,
          params->qhs > 0 ? params->qhs : 0.707, params->ghs, 0.0, 0.0,
          STEEPNESS_TYPE_Q, sample_rate);
      break;
    }
  }

  filter->num_sections = num;
  filter->sections = (biquad_filter_t**)malloc(num * sizeof(biquad_filter_t*));
  for (size_t i = 0; i < num; i++) {
    filter->sections[i] = secs[i];
  }
  return filter;
}

void biquad_combo_filter_process(biquad_combo_filter_t* filter,
                                 mutable_waveform_t waveform, size_t count) {
  if (!filter || !waveform || count == 0) return;
  for (size_t i = 0; i < filter->num_sections; i++) {
    if (filter->sections[i]) {
      biquad_filter_process(filter->sections[i], waveform, count);
    }
  }
}

void biquad_combo_filter_update_parameters(biquad_combo_filter_t* filter,
                                           const filter_config_t* config,
                                           int sample_rate) {
  if (!filter || !config) return;
  if (config->type != FILTER_TYPE_BIQUAD_COMBO) return;
  for (size_t i = 0; i < filter->num_sections; i++) {
    if (filter->sections[i]) biquad_filter_free(filter->sections[i]);
  }
  free(filter->sections);
  biquad_combo_filter_t* tmp = biquad_combo_filter_create(
      filter->name, &config->parameters.biquad_combo, sample_rate);
  if (tmp) {
    filter->sections = tmp->sections;
    filter->num_sections = tmp->num_sections;
    free(tmp);
  } else {
    filter->sections = NULL;
    filter->num_sections = 0;
  }
}

void biquad_combo_filter_free(biquad_combo_filter_t* filter) {
  if (!filter) return;
  for (size_t i = 0; i < filter->num_sections; i++) {
    if (filter->sections[i]) biquad_filter_free(filter->sections[i]);
  }
  if (filter->sections) free(filter->sections);
  free(filter);
}
