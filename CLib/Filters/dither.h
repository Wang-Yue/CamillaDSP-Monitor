#ifndef CLIB_FILTERS_DITHER_H
#define CLIB_FILTERS_DITHER_H

#include "Audio/prc_fmt.h"
#include "Config/filter_config_types.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// MARK: - Ditherers

// MARK: - NoiseShaper
typedef struct {
    prc_fmt_t* filter;
    prc_fmt_t* buffer;
    size_t filter_count;
    size_t write_index;
} noise_shaper_t;

noise_shaper_t* noise_shaper_create(const prc_fmt_t* filter_coeffs, size_t count);
prc_fmt_t noise_shaper_process(noise_shaper_t* shaper, prc_fmt_t scaled, prc_fmt_t dither);
void noise_shaper_free(noise_shaper_t* shaper);

// MARK: - Noise Shaper Factory
noise_shaper_t* noise_shaper_create_for_type(dither_type_t type);

// MARK: - DitherFilter
typedef struct {
    char name[64];
    dither_type_t type;
    prc_fmt_t scalefact;
    prc_fmt_t amplitude;
    noise_shaper_t* shaper;
    prc_fmt_t previous_sample;
} dither_filter_t;

dither_filter_t* dither_filter_create(const char* name, const dither_parameters_t* params);
void dither_filter_process(dither_filter_t* filter, mutable_waveform_t waveform, size_t count);
void dither_filter_update_parameters(dither_filter_t* filter, const filter_config_t* config, int sample_rate);
void dither_filter_free(dither_filter_t* filter);

#ifdef __cplusplus
}
#endif

#endif // CLIB_FILTERS_DITHER_H
