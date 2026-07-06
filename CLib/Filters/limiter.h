#ifndef CLIB_FILTERS_LIMITER_H
#define CLIB_FILTERS_LIMITER_H

#include "Audio/prc_fmt.h"
#include "Config/filter_config_types.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[64];
    prc_fmt_t clip_limit;
    bool soft_clip;
} limiter_filter_t;

limiter_filter_t* limiter_filter_create(const char* name, const limiter_parameters_t* params);
void limiter_filter_process(limiter_filter_t* filter, mutable_waveform_t waveform, size_t count);
void limiter_filter_update_parameters(limiter_filter_t* filter, const filter_config_t* config, int sample_rate);
void limiter_filter_free(limiter_filter_t* filter);

#ifdef __cplusplus
}
#endif

#endif // CLIB_FILTERS_LIMITER_H
