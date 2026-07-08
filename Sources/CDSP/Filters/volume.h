#ifndef CLIB_FILTERS_VOLUME_H
#define CLIB_FILTERS_VOLUME_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Audio/processing_parameters.h"
#include "Config/filter_config_types.h"

typedef struct volume_filter volume_filter_t;

volume_filter_t* volume_filter_create(const char* name,
                                      const volume_parameters_t* params,
                                      int sample_rate, size_t chunk_size,
                                      processing_parameters_t* proc_params);

/// Pre-calculates target volume levels and generates ramping array once per
/// chunk. Must be called once per audio chunk before processing individual
/// channel waveforms.
void volume_filter_prepare_chunk(volume_filter_t* filter);

/// Conforms to `Filter`. Processes a single channel's waveform slice.
void volume_filter_process(volume_filter_t* filter, mutable_waveform_t waveform,
                           size_t count);

/// Advances the fader's ramp steps.
/// Must be called once per audio chunk after all channels have been processed.
void volume_filter_advance_ramp(volume_filter_t* filter);
void volume_filter_update_parameters(volume_filter_t* filter,
                                     const filter_config_t* config,
                                     int sample_rate);
void volume_filter_free(volume_filter_t* filter);

#endif  // CLIB_FILTERS_VOLUME_H
