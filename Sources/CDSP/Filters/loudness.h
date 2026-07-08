#ifndef CLIB_FILTERS_LOUDNESS_H
#define CLIB_FILTERS_LOUDNESS_H

// RME ADI-2 DAC Loudness Curves
// https://www.rme-audio.de/downloads/adi2dac_e.pdf

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Audio/processing_parameters.h"
#include "Config/filter_config_types.h"


typedef struct loudness_filter loudness_filter_t;

loudness_filter_t* loudness_filter_create(const char* name,
                                          const loudness_parameters_t* params,
                                          int sample_rate,
                                          processing_parameters_t* proc_params);
void loudness_filter_process(loudness_filter_t* filter,
                             mutable_waveform_t waveform, size_t count);
void loudness_filter_update_parameters(loudness_filter_t* filter,
                                       const filter_config_t* config,
                                       int sample_rate);
void loudness_filter_free(loudness_filter_t* filter);

#endif  // CLIB_FILTERS_LOUDNESS_H
