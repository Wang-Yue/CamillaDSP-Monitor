#ifndef CLIB_FILTERS_LOOKAHEAD_LIMITER_H
#define CLIB_FILTERS_LOOKAHEAD_LIMITER_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/config_error.h"
#include "Config/filter_config_types.h"

typedef struct lookahead_limiter_filter lookahead_limiter_filter_t;

int lookahead_limiter_parameters_validate(
    const lookahead_limiter_parameters_t* params, int sample_rate,
    config_error_t* err);
lookahead_limiter_filter_t* lookahead_limiter_filter_create(
    const char* name, const lookahead_limiter_parameters_t* params,
    int sample_rate, size_t chunk_size);
void lookahead_limiter_filter_process(lookahead_limiter_filter_t* filter,
                                      mutable_waveform_t waveform,
                                      size_t count);
void lookahead_limiter_filter_update_parameters(
    lookahead_limiter_filter_t* filter, const filter_config_t* config,
    int sample_rate);
void lookahead_limiter_filter_free(lookahead_limiter_filter_t* filter);

#endif  // CLIB_FILTERS_LOOKAHEAD_LIMITER_H
