#ifndef CLIB_CONFIG_RESAMPLER_CONFIG_TYPES_H
#define CLIB_CONFIG_RESAMPLER_CONFIG_TYPES_H

// Standalone resampler configuration types.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RESAMPLER_TYPE_SYNCHRONOUS = 0,
    RESAMPLER_TYPE_APPLE,
    RESAMPLER_TYPE_ASYNC_SINC,
    RESAMPLER_TYPE_ASYNC_POLY
} resampler_type_t;

/// Quality settings supported by Apple's AudioConverter.
typedef enum {
    APPLE_RESAMPLER_QUALITY_MIN = 0,
    APPLE_RESAMPLER_QUALITY_LOW,
    APPLE_RESAMPLER_QUALITY_MEDIUM,
    APPLE_RESAMPLER_QUALITY_HIGH,
    APPLE_RESAMPLER_QUALITY_MAX
} apple_resampler_quality_t;

/// Algorithm complexity supported by Apple's AudioConverter.
typedef enum {
    APPLE_RESAMPLER_COMPLEXITY_LINEAR = 0,
    APPLE_RESAMPLER_COMPLEXITY_NORMAL,
    APPLE_RESAMPLER_COMPLEXITY_MASTERING,
    APPLE_RESAMPLER_COMPLEXITY_MINIMUM_PHASE
} apple_resampler_complexity_t;

typedef enum {
    RESAMPLER_PROFILE_VERY_FAST = 0,
    RESAMPLER_PROFILE_FAST,
    RESAMPLER_PROFILE_BALANCED,
    RESAMPLER_PROFILE_ACCURATE
} resampler_profile_t;

typedef struct {
    resampler_type_t type;
    char profile[32];
    bool has_profile;
    char interpolation[32];
    bool has_interpolation;
    apple_resampler_quality_t apple_quality;
    bool has_apple_quality;
    apple_resampler_complexity_t apple_complexity;
    bool has_apple_complexity;
    int sinc_len;
    bool has_sinc_len;
    int oversampling_factor;
    bool has_oversampling_factor;
    char window[32];
    bool has_window;
    double f_cutoff;
    bool has_f_cutoff;
} resampler_config_t;

const char* resampler_type_to_string(resampler_type_t type);
resampler_type_t resampler_type_from_string(const char* str);

const char* apple_resampler_quality_to_string(apple_resampler_quality_t quality);
apple_resampler_quality_t apple_resampler_quality_from_string(const char* str);

const char* apple_resampler_complexity_to_string(apple_resampler_complexity_t comp);
apple_resampler_complexity_t apple_resampler_complexity_from_string(const char* str);
uint32_t apple_resampler_complexity_os_type(apple_resampler_complexity_t comp);

const char* resampler_profile_to_string(resampler_profile_t profile);
resampler_profile_t resampler_profile_from_string(const char* str);

void resampler_config_init(resampler_config_t* config, resampler_type_t type);
void resampler_config_description(const resampler_config_t* config, char* out_buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif // CLIB_CONFIG_RESAMPLER_CONFIG_TYPES_H
