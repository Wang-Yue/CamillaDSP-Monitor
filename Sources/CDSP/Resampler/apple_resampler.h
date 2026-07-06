// Apple AudioConverter resampler.

#ifndef CLIB_RESAMPLER_APPLE_RESAMPLER_H
#define CLIB_RESAMPLER_APPLE_RESAMPLER_H

#include <stddef.h>
#include <stdbool.h>
#include "Audio/audio_chunk.h"
#include "resampler_error.h"
#include "Config/resampler_config_types.h"

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    audio_buffers_t* buffers;
    size_t read_offset;
    size_t write_offset;
} apple_resampler_fill_context_t;

typedef struct {
    size_t channels;
    size_t chunk_size;
    double base_ratio;
    double current_ratio;
#ifdef __APPLE__
    AudioConverterRef converter;
#else
    void* converter;
#endif
    apple_resampler_fill_context_t* fill_context;
    void* abl_storage;
    size_t max_output_frames;
} apple_resampler_t;

apple_resampler_t* apple_resampler_create(size_t channels, size_t input_rate, size_t output_rate, apple_resampler_quality_t quality, apple_resampler_complexity_t complexity, size_t chunk_size);
void apple_resampler_free(apple_resampler_t* resampler);

resampler_error_t apple_resampler_process(apple_resampler_t* resampler, const audio_chunk_t* input, audio_chunk_t* output);
void apple_resampler_set_relative_ratio(apple_resampler_t* resampler, double multiplier);
double apple_resampler_get_ratio(const apple_resampler_t* resampler);
size_t apple_resampler_get_max_output_frames(const apple_resampler_t* resampler);
size_t apple_resampler_get_chunk_size(const apple_resampler_t* resampler);
size_t apple_resampler_get_channels(const apple_resampler_t* resampler);

#ifdef __cplusplus
}
#endif

#endif // CLIB_RESAMPLER_APPLE_RESAMPLER_H
