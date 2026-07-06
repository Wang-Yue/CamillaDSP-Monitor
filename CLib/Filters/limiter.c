#include "limiter.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif

limiter_filter_t* limiter_filter_create(const char* name, const limiter_parameters_t* params) {
    limiter_filter_t* filter = (limiter_filter_t*)malloc(sizeof(limiter_filter_t));
    if (!filter) return NULL;
    if (name) {
        strncpy(filter->name, name, sizeof(filter->name) - 1);
        filter->name[sizeof(filter->name) - 1] = '\0';
    } else {
        strcpy(filter->name, "limiter");
    }
    double limit_db = params ? params->clip_limit : 0.0;
    filter->clip_limit = prc_fmt_from_db(limit_db);
    filter->soft_clip = params ? params->soft_clip : false;
    return filter;
}

void limiter_filter_process(limiter_filter_t* filter, mutable_waveform_t waveform, size_t count) {
    if (!filter || !waveform || count == 0) return;
    if (filter->soft_clip) {
        double inv_limit = 1.0 / filter->clip_limit;
        for (size_t i = 0; i < count; i++) {
            double scaled = waveform[i] * inv_limit;
            if (scaled < -1.5) scaled = -1.5;
            else if (scaled > 1.5) scaled = 1.5;
            waveform[i] = (scaled - (scaled * scaled * scaled) / 6.75) * filter->clip_limit;
        }
    } else {
        double low_limit = -filter->clip_limit;
        double high_limit = filter->clip_limit;
#ifdef __APPLE__
        vDSP_vclipD(waveform, 1, &low_limit, &high_limit, waveform, 1, count);
#else
        for (size_t i = 0; i < count; i++) {
            if (waveform[i] < low_limit) waveform[i] = low_limit;
            else if (waveform[i] > high_limit) waveform[i] = high_limit;
        }
#endif
    }
}

void limiter_filter_update_parameters(limiter_filter_t* filter, const filter_config_t* config, int sample_rate) {
    (void)sample_rate;
    if (!filter || !config) return;
    if (config->type != FILTER_TYPE_LIMITER) return;
    const limiter_parameters_t* params = &config->parameters.limiter;
    filter->clip_limit = prc_fmt_from_db(params->clip_limit);
    filter->soft_clip = params->soft_clip;
}

void limiter_filter_free(limiter_filter_t* filter) {
    if (filter) free(filter);
}
