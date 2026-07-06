#ifndef CLIB_FILTERS_GAIN_H
#define CLIB_FILTERS_GAIN_H

#include "Audio/prc_fmt.h"
#include "Config/filter_config_types.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[64];
    prc_fmt_t linear_gain;
    bool muted;
} gain_filter_t;

gain_filter_t* gain_filter_create(const char* name, const gain_parameters_t* params);
void gain_filter_process(gain_filter_t* filter, mutable_waveform_t waveform, size_t count);
prc_fmt_t gain_filter_process_single(gain_filter_t* filter, prc_fmt_t sample);
void gain_filter_update_parameters(gain_filter_t* filter, const filter_config_t* config, int sample_rate);
void gain_filter_free(gain_filter_t* filter);

#ifdef __cplusplus
}
#endif

#endif // CLIB_FILTERS_GAIN_H
