#ifndef CLIB_FILTERS_FILTER_H
#define CLIB_FILTERS_FILTER_H

#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Audio/processing_parameters.h"
#include "Config/filter_config_types.h"
#include "biquad.h"
#include "biquad_combo.h"
#include "convolution.h"
#include "delay.h"
#include "diffeq.h"
#include "dither.h"
#include "gain.h"
#include "limiter.h"
#include "lookahead_limiter.h"
#include "loudness.h"
#include "volume.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  FILTER_INSTANCE_BIQUAD,
  FILTER_INSTANCE_BIQUAD_COMBO,
  FILTER_INSTANCE_CONVOLUTION,
  FILTER_INSTANCE_DELAY,
  FILTER_INSTANCE_DIFF_EQ,
  FILTER_INSTANCE_DITHER,
  FILTER_INSTANCE_GAIN,
  FILTER_INSTANCE_LIMITER,
  FILTER_INSTANCE_LOOKAHEAD_LIMITER,
  FILTER_INSTANCE_LOUDNESS,
  FILTER_INSTANCE_VOLUME
} filter_instance_type_t;

/// Protocol for all audio filters. Filters operate on one channel at a time.
///
/// `waveform` is a pointer into class-owned storage (`AudioBuffers`). The
/// pointer's `count` is the number of samples to process — typically the
/// owning chunk's `validFrames`, sliced down by the caller. Filters must
/// not assume the pointer covers the channel's full capacity.
typedef struct {
  /// The unique name of this filter instance.
  char name[64];
  filter_instance_type_t type;
  void* instance;
} filter_t;

/// Factory to create filter instances from configuration.
///
/// Validation runs first via `FilterConfig.validate(sampleRate:)`; the
/// switch then constructs the runtime filter for each variant. The
/// `.volume` case is reserved for the implicit master-volume filter
/// inside `Pipeline` and cannot be user-defined.
filter_t* filter_create(const char* name, const filter_config_t* config,
                        int sample_rate, size_t chunk_size,
                        processing_parameters_t* proc_params);

/// Process a waveform buffer in-place. The buffer's `count` defines the
/// processed range.
void filter_process(filter_t* filter, mutable_waveform_t waveform,
                    size_t count);

/// Update the filter parameters dynamically.
void filter_update_parameters(filter_t* filter, const filter_config_t* config,
                              int sample_rate);
void filter_free(filter_t* filter);

#ifdef __cplusplus
}
#endif

#endif  // CLIB_FILTERS_FILTER_H
