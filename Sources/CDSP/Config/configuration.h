/**
 * @file configuration.h
 * @brief Top-level configuration data structures and validation logic.
 *
 * This file owns:
 *   1. Top-level configuration models (dsp_config_t and pipeline_step_t).
 *   2. Cross-component validation logic, including schema checks and the
 *      pipeline walk that tracks channel layouts.
 */

#ifndef CLIB_CONFIG_CONFIGURATION_H
#define CLIB_CONFIG_CONFIGURATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config_error.h"
#include "engine_config_types.h"
#include "filter_config_types.h"
#include "mixer_config_types.h"
#include "processor_config_types.h"

/**
 * @brief Type of pipeline step.
 */
typedef enum {
  PIPELINE_STEP_TYPE_FILTER = 0, /**< Step applies a filter. */
  PIPELINE_STEP_TYPE_MIXER,      /**< Step applies a mixer. */
  PIPELINE_STEP_TYPE_PROCESSOR   /**< Step applies a processor. */
} pipeline_step_type_t;

/**
 * @brief One step in the user-defined processing pipeline.
 *
 * Either a named filter chain applied to one or more channels,
 * or a mixer that changes the channel layout.
 */
typedef struct {
  pipeline_step_type_t type; /**< The type of pipeline step. */
  int channel; /**< The channel to apply the filter to (if single channel). */
  bool has_channel;      /**< True if `channel` is valid. */
  int* channels;         /**< Array of channels (if multi-channel). */
  size_t channels_count; /**< Number of channels in `channels`. */
  char name[128];        /**< Name of the filter, mixer, or processor. */
  bool has_name;         /**< True if `name` is valid. */
  char** names;          /**< Array of names (if multi-name). */
  size_t names_count;    /**< Number of names in `names`. */
  bool bypassed;         /**< True if this step is bypassed. */
} pipeline_step_t;

/**
 * @brief Named filter configuration.
 */
typedef struct {
  char name[128];         /**< Name of the filter. */
  filter_config_t filter; /**< Filter configuration. */
} named_filter_config_t;

/**
 * @brief Named mixer configuration.
 */
typedef struct {
  char name[128];       /**< Name of the mixer. */
  mixer_config_t mixer; /**< Mixer configuration. */
} named_mixer_config_t;

/**
 * @brief Named processor configuration.
 */
typedef struct {
  char name[128];               /**< Name of the processor. */
  processor_config_t processor; /**< Processor configuration. */
} named_processor_config_t;

/**
 * @brief Top-level configuration consumed by the DSP engine.
 */
typedef struct {
  devices_config_t devices;             /**< Audio devices configuration. */
  named_filter_config_t* filters;       /**< Array of named filters. */
  size_t filters_count;                 /**< Number of filters. */
  named_mixer_config_t* mixers;         /**< Array of named mixers. */
  size_t mixers_count;                  /**< Number of mixers. */
  named_processor_config_t* processors; /**< Array of named processors. */
  size_t processors_count;              /**< Number of processors. */
  pipeline_step_t*
      pipeline; /**< Array of pipeline steps defining the processing flow. */
  size_t pipeline_count; /**< Number of pipeline steps. */
} dsp_config_t;

/**
 * @brief Validates the DSP configuration.
 *
 * Checks schema validity and performs a pipeline walk to verify channel
 * layouts.
 *
 * @param config Pointer to the configuration to validate.
 * @param err Pointer to a config_error_t struct to receive error details if
 * validation fails.
 * @return 0 if valid, non-zero if invalid.
 */
int dsp_config_validate(const dsp_config_t* config, config_error_t* err);

/**
 * @brief Parses a JSON string into a DSP configuration.
 *
 * Allocates and populates a dsp_config_t structure.
 *
 * @param json The JSON string to parse.
 * @param out_config Pointer to a pointer to receive the allocated
 * configuration.
 * @param err Pointer to a config_error_t struct to receive error details if
 * parsing fails.
 * @return 0 on success, non-zero on failure.
 */
int dsp_config_parse_json(const char* json, dsp_config_t** out_config,
                          config_error_t* err);

/**
 * @brief Frees a DSP configuration.
 *
 * Deallocates the configuration structure and all associated nested structures.
 *
 * @param config Pointer to the configuration to free.
 */
void dsp_config_free(dsp_config_t* config);

/**
 * @brief Retrieves a filter configuration by name.
 *
 * @param config Pointer to the top-level configuration.
 * @param name Name of the filter to retrieve.
 * @return Pointer to the filter configuration, or NULL if not found.
 */
filter_config_t* dsp_config_get_filter(const dsp_config_t* config,
                                       const char* name);

/**
 * @brief Retrieves a mixer configuration by name.
 *
 * @param config Pointer to the top-level configuration.
 * @param name Name of the mixer to retrieve.
 * @return Pointer to the mixer configuration, or NULL if not found.
 */
mixer_config_t* dsp_config_get_mixer(const dsp_config_t* config,
                                     const char* name);

/**
 * @brief Retrieves a processor configuration by name.
 *
 * @param config Pointer to the top-level configuration.
 * @param name Name of the processor to retrieve.
 * @return Pointer to the processor configuration, or NULL if not found.
 */
processor_config_t* dsp_config_get_processor(const dsp_config_t* config,
                                             const char* name);

#endif  // CLIB_CONFIG_CONFIGURATION_H
