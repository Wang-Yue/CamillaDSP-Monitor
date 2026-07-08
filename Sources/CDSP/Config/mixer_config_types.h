#ifndef CLIB_CONFIG_MIXER_CONFIG_TYPES_H
#define CLIB_CONFIG_MIXER_CONFIG_TYPES_H

/**
 * @file mixer_config_types.h
 * @brief Configuration types and functions for CamillaDSP mixers.
 *
 * This file defines the structures and functions used to configure
 * mixers in CamillaDSP, including sources, mappings, and validation.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config_error.h"
#include "filter_config_types.h"

/**
 * @brief Represents a source channel in a mixer mapping.
 */
typedef struct {
  size_t channel; /**< The input channel index. */
  double gain;   /**< Gain value. Optional (defaults to 0.0 dB when omitted). */
  bool has_gain; /**< Flag indicating if gain is specified. */
  bool inverted; /**< Flag indicating if the phase is inverted. */
  bool mute;     /**< Flag indicating if the source is muted. */
  gain_scale_t scale; /**< The scale type for the gain value. */
} mixer_source_t;

/**
 * @brief Represents a mapping of source channels to a destination channel.
 */
typedef struct {
  size_t dest; /**< The destination channel index. */
  union {
    size_t sources_count; /**< Number of source channels. */
    size_t num_sources;   /**< Alias for sources_count. */
  };
  mixer_source_t* sources; /**< Array of source channels. */
  bool mute; /**< Flag indicating if the destination channel is muted. */
} mixer_mapping_t;

/**
 * @brief Represents the configuration for a mixer.
 *
 * Supports both nested format `channels: { in: N, out: N }` and
 * flat format `channels_in: N, channels_out: N`.
 */
typedef struct {
  union {
    size_t channels_in; /**< Number of input channels (flat/nested format). */
    size_t num_channels_in; /**< Alias for channels_in. */
  };
  union {
    size_t channels_out; /**< Number of output channels (flat/nested format). */
    size_t num_channels_out; /**< Alias for channels_out. */
  };
  union {
    size_t mapping_count; /**< Number of mappings. */
    size_t num_mappings;  /**< Alias for mapping_count. */
  };
  union {
    mixer_mapping_t* mapping;  /**< Array of mappings. */
    mixer_mapping_t* mappings; /**< Alias for mapping. */
  };
  char description[256]; /**< Description of the mixer. */
} mixer_config_t;

/**
 * @brief Convenience accessor: 0.0 when gain is nil.
 *
 * @param src Pointer to the mixer source.
 * @return The gain value.
 */
double mixer_source_gain_value(const mixer_source_t* src);

/**
 * @brief Validate the mapping is internally consistent.
 *
 * Checks that every dest is in range, no dest appears twice, and within
 * a single dest no source channel appears twice.
 *
 * @param mixer Pointer to the mixer configuration.
 * @param err Pointer to config_error_t to store error details if validation
 * fails.
 * @return 0 if valid, non-zero error code otherwise.
 */
int mixer_config_validate(const mixer_config_t* mixer, config_error_t* err);

#endif  // CLIB_CONFIG_MIXER_CONFIG_TYPES_H
