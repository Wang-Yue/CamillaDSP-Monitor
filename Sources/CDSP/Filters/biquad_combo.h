#ifndef CLIB_FILTERS_BIQUAD_COMBO_H
#define CLIB_FILTERS_BIQUAD_COMBO_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/config_error.h"
#include "Config/filter_config_types.h"


typedef struct biquad_combo_filter biquad_combo_filter_t;

int biquad_combo_parameters_validate(const biquad_combo_parameters_t* params,
                                     int sample_rate, config_error_t* err);
// MARK: - Butterworth & Linkwitz-Riley helper calculations
size_t biquad_combo_butterworth_q(int order, double* out_q, size_t max_q);
size_t biquad_combo_linkwitz_riley_q(int order, double* out_q, size_t max_q);

// MARK: - Tilt EQ
// MARK: - Graphic EQ
// MARK: - Five Point PEQ
biquad_combo_filter_t* biquad_combo_filter_create(
    const char* name, const biquad_combo_parameters_t* params, int sample_rate);
void biquad_combo_filter_process(biquad_combo_filter_t* filter,
                                 mutable_waveform_t waveform, size_t count);
void biquad_combo_filter_update_parameters(biquad_combo_filter_t* filter,
                                           const filter_config_t* config,
                                           int sample_rate);
void biquad_combo_filter_free(biquad_combo_filter_t* filter);

#endif  // CLIB_FILTERS_BIQUAD_COMBO_H
