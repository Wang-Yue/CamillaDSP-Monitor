#ifndef CLIB_CONFIG_PROCESSOR_CONFIG_TYPES_H
#define CLIB_CONFIG_PROCESSOR_CONFIG_TYPES_H

/**
 * @file processor_config_types.h
 * @brief Configuration types and functions for CamillaDSP processors.
 *
 * This file defines the structures, enums, and functions used to configure
 * processors in CamillaDSP, such as compressors, noise gates, and RACE.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config_error.h"
#include "filter_config_types.h"

/**
 * @brief Supported processor types.
 */
typedef enum {
  PROCESSOR_TYPE_COMPRESSOR = 0, /**< Compressor processor. */
  PROCESSOR_TYPE_NOISE_GATE,     /**< Noise gate processor. */
  PROCESSOR_TYPE_RACE /**< RACE (Receiver Active Crosstalk Cancellation)
                         processor. */
} processor_type_t;

/**
 * @brief Convert a processor type enum to its string representation.
 *
 * @param type The processor type.
 * @return The string representation of the processor type.
 */
const char* processor_type_to_string(processor_type_t type);

/**
 * @brief Convert a string representation to a processor type enum.
 *
 * @param str The string representation.
 * @return The corresponding processor type enum.
 */
processor_type_t processor_type_from_string(const char* str);

/**
 * @brief Parameters for the Compressor processor.
 */
typedef struct {
  int channels;                  /**< Number of channels. */
  int* monitor_channels;         /**< Array of monitor channels. */
  size_t monitor_channels_count; /**< Number of monitor channels. */
  int* process_channels;         /**< Array of channels to process. */
  size_t process_channels_count; /**< Number of process channels. */
  double attack;                 /**< Attack time. */
  double release;                /**< Release time. */
  double threshold;              /**< Threshold level. */
  double factor;                 /**< Compression factor/ratio. */
  double makeup_gain;            /**< Makeup gain value. */
  bool has_makeup_gain; /**< Flag indicating if makeup gain is specified. */
  bool soft_clip;       /**< Flag indicating if soft clipping is enabled. */
  double clip_limit;    /**< Soft clip limit. */
  bool has_clip_limit;  /**< Flag indicating if soft clip limit is specified. */
} compressor_parameters_t;

/**
 * @brief Parameters for the Noise Gate processor.
 */
typedef struct {
  int channels;                  /**< Number of channels. */
  int* monitor_channels;         /**< Array of monitor channels. */
  size_t monitor_channels_count; /**< Number of monitor channels. */
  int* process_channels;         /**< Array of channels to process. */
  size_t process_channels_count; /**< Number of process channels. */
  double attack;                 /**< Attack time. */
  double release;                /**< Release time. */
  double threshold;              /**< Threshold level. */
  double attenuation;            /**< Attenuation level. */
} noise_gate_parameters_t;

/**
 * @brief Parameters for the RACE processor.
 */
typedef struct {
  int channels;         /**< Number of channels. */
  int channel_a;        /**< Channel A index. */
  int channel_b;        /**< Channel B index. */
  double delay;         /**< Delay value. */
  bool subsample_delay; /**< Flag indicating if subsample delay is enabled. */
  bool has_subsample_delay; /**< Flag indicating if subsample_delay is
                               specified. */
  delay_unit_t delay_unit;  /**< Unit of the delay value. */
  bool has_delay_unit;      /**< Flag indicating if delay_unit is specified. */
  double attenuation;       /**< Attenuation level. */
} race_parameters_t;

/**
 * @brief Configuration for a processor, containing its type and parameters.
 */
typedef struct {
  processor_type_t type; /**< The type of the processor. */
  union {
    compressor_parameters_t compressor; /**< Compressor parameters. */
    noise_gate_parameters_t noise_gate; /**< Noise gate parameters. */
    race_parameters_t race;             /**< RACE parameters. */
  } parameters;                         /**< Union of processor parameters. */
} processor_config_t;

/**
 * @brief Validate a processor configuration.
 *
 * @param proc Pointer to the processor configuration to validate.
 * @param err Pointer to config_error_t to store error details if validation
 * fails.
 * @return 0 if valid, non-zero error code otherwise.
 */
int processor_config_validate(const processor_config_t* proc,
                              config_error_t* err);

#endif  // CLIB_CONFIG_PROCESSOR_CONFIG_TYPES_H
