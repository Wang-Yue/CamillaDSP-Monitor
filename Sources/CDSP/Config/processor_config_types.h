#ifndef CLIB_CONFIG_PROCESSOR_CONFIG_TYPES_H
#define CLIB_CONFIG_PROCESSOR_CONFIG_TYPES_H

#include "config_error.h"
#include "filter_config_types.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROCESSOR_TYPE_COMPRESSOR = 0,
    PROCESSOR_TYPE_NOISE_GATE,
    PROCESSOR_TYPE_RACE
} processor_type_t;

const char* processor_type_to_string(processor_type_t type);
processor_type_t processor_type_from_string(const char* str);

typedef struct {
    int channels;
    int* monitor_channels;
    size_t monitor_channels_count;
    int* process_channels;
    size_t process_channels_count;
    double attack;
    double release;
    double threshold;
    double factor;
    double makeup_gain;
    bool has_makeup_gain;
    bool soft_clip;
    double clip_limit;
    bool has_clip_limit;
} compressor_parameters_t;

typedef struct {
    int channels;
    int* monitor_channels;
    size_t monitor_channels_count;
    int* process_channels;
    size_t process_channels_count;
    double attack;
    double release;
    double threshold;
    double attenuation;
} noise_gate_parameters_t;

typedef struct {
    int channels;
    int channel_a;
    int channel_b;
    double delay;
    bool subsample_delay;
    bool has_subsample_delay;
    delay_unit_t delay_unit;
    bool has_delay_unit;
    double attenuation;
} race_parameters_t;

typedef struct {
    processor_type_t type;
    union {
        compressor_parameters_t compressor;
        noise_gate_parameters_t noise_gate;
        race_parameters_t race;
    } parameters;
} processor_config_t;

int processor_config_validate(const processor_config_t* proc, config_error_t* err);

#ifdef __cplusplus
}
#endif

#endif // CLIB_CONFIG_PROCESSOR_CONFIG_TYPES_H
