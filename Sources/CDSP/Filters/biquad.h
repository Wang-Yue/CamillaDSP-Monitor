#ifndef CLIB_FILTERS_BIQUAD_H
#define CLIB_FILTERS_BIQUAD_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/config_error.h"
#include "Config/filter_config_types.h"

#ifdef ENABLE_ACCELERATE
#include <Accelerate/Accelerate.h>
#endif

typedef struct {
  double b0;
  double b1;
  double b2;
  double a1;
  double a2;
} biquad_coefficients_t;

static inline biquad_coefficients_t biquad_coefficients_passthrough(void) {
  biquad_coefficients_t c = {1.0, 0.0, 0.0, 0.0, 0.0};
  return c;
}

bool biquad_coefficients_compute(const biquad_parameters_t* params,
                                 int sample_rate,
                                 biquad_coefficients_t* out_coeffs);

/// Magnitude response in dB at frequency `f` (Hz). Uses the analytic
/// transfer function H(z=e^{jω}) — no time-domain simulation needed.
/// Returns 0 dB for the degenerate case where the denominator
/// vanishes.
double biquad_coefficients_gain_db(const biquad_coefficients_t* coeffs,
                                   double f, int sample_rate);

/// Phase response in radians at frequency `f` (Hz), wrapped to
/// `(−π, π]`. Sign convention matches `atan2(Im(H), Re(H))`.
double biquad_coefficients_phase_rad(const biquad_coefficients_t* coeffs,
                                     double f, int sample_rate);

typedef struct {
  char name[64];
  biquad_coefficients_t coeffs;
#ifdef ENABLE_ACCELERATE
  vDSP_biquadm_SetupD setup;
  double coeffs_array[5];
#else
  double z1, z2;
#endif
} biquad_filter_t;

biquad_filter_t* biquad_filter_create(const char* name,
                                      const biquad_coefficients_t* coeffs);
void biquad_filter_process(biquad_filter_t* filter, mutable_waveform_t waveform,
                           size_t count);
double biquad_filter_process_single(biquad_filter_t* filter, double sample);
void biquad_filter_update_parameters(biquad_filter_t* filter,
                                     const filter_config_t* config,
                                     int sample_rate);
void biquad_filter_free(biquad_filter_t* filter);

#endif  // CLIB_FILTERS_BIQUAD_H
