#ifndef CLIB_CONFIG_MIXER_CONFIG_TYPES_H
#define CLIB_CONFIG_MIXER_CONFIG_TYPES_H

// Standalone mixer configuration types.

#include "config_error.h"
#include "filter_config_types.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t channel;
    /// Gain value. Optional (defaults to 0.0 dB when omitted).
    double gain;
    bool has_gain;
    bool inverted;
    bool mute;
    gain_scale_t scale;
} mixer_source_t;

typedef struct {
    size_t dest;
    union {
        size_t sources_count;
        size_t num_sources;
    };
    mixer_source_t* sources;
    bool mute;
} mixer_mapping_t;

// Support both nested format `channels: { in: N, out: N }` and
// flat format `channels_in: N, channels_out: N`
typedef struct {
    union {
        size_t channels_in; // flat format (or nested format `channels: { in: N, out: N }`)
        size_t num_channels_in;
    };
    union {
        size_t channels_out; // flat format (or nested format `channels: { in: N, out: N }`)
        size_t num_channels_out;
    };
    union {
        size_t mapping_count;
        size_t num_mappings;
    };
    union {
        mixer_mapping_t* mapping;
        mixer_mapping_t* mappings;
    };
    char description[256];
} mixer_config_t;

/// Convenience accessor: 0.0 when gain is nil
double mixer_source_gain_value(const mixer_source_t* src);
/// Validate the mapping is internally consistent: every dest is in
/// range, no dest appears twice, and within a single dest no source
/// channel appears twice.
int mixer_config_validate(const mixer_config_t* mixer, config_error_t* err);

#ifdef __cplusplus
}
#endif

#endif // CLIB_CONFIG_MIXER_CONFIG_TYPES_H
