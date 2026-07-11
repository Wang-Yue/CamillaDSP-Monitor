/**
 * @file config_diff.h
 * @brief Configuration difference detection and reporting.
 *
 * This file provides functions to compare two DSP configurations and
 * identify the types of changes (e.g., filter parameters, mixer parameters,
 * pipeline, devices) and retrieve the names of changed elements.
 */

#ifndef CLIB_CONFIG_CONFIG_DIFF_H
#define CLIB_CONFIG_CONFIG_DIFF_H

#include <stddef.h>

#include "configuration.h"

/**
 * @enum config_change_type_t
 * @brief Identifies the type of configuration change.
 */
typedef enum {
  CONFIG_CHANGE_NONE = 0,          /**< No changes detected. */
  CONFIG_CHANGE_FILTER_PARAMETERS, /**< Filter parameters changed. */
  CONFIG_CHANGE_MIXER_PARAMETERS,  /**< Mixer parameters changed. */
  CONFIG_CHANGE_PIPELINE,          /**< Pipeline configuration changed. */
  CONFIG_CHANGE_DEVICES            /**< Device configuration changed. */
} config_change_type_t;

/**
 * @typedef config_change_t
 * @brief Opaque structure containing details about configuration changes.
 */
typedef struct config_change config_change_t;

/**
 * @brief Creates a new config_change_t instance.
 *
 * @return A pointer to the created config_change_t, or NULL on failure.
 */
config_change_t* config_change_create(void);

/**
 * @brief Frees a config_change_t instance.
 *
 * @param change Pointer to the config_change_t instance to free.
 */
void config_change_free(config_change_t* change);

/**
 * @brief Compares two configurations and populates change details.
 *
 * @param current The current configuration.
 * @param new_conf The new configuration to compare against.
 * @param change Pointer to a config_change_t to be populated with details.
 * @return The highest severity change type detected.
 */
config_change_type_t config_diff(const dsp_config_t* current,
                                 const dsp_config_t* new_conf,
                                 config_change_t* change);

/**
 * @brief Compares two devices configurations for equality.
 * @param a Pointer to first devices configuration.
 * @param b Pointer to second devices configuration.
 * @return true if configurations are equal, false otherwise.
 */
bool devices_config_equal(const devices_config_t* a, const devices_config_t* b);

/**
 * @brief Takes ownership of the list of changed filter names.
 *
 * The caller is responsible for freeing the returned array of strings.
 *
 * @param change Pointer to the config_change_t instance.
 * @param out_count Pointer to store the number of filter names returned.
 * @return Array of string pointers containing the changed filter names.
 */
char** config_change_take_filters(config_change_t* change, size_t* out_count);

/**
 * @brief Takes ownership of the list of changed mixer names.
 *
 * The caller is responsible for freeing the returned array of strings.
 *
 * @param change Pointer to the config_change_t instance.
 * @param out_count Pointer to store the number of mixer names returned.
 * @return Array of string pointers containing the changed mixer names.
 */
char** config_change_take_mixers(config_change_t* change, size_t* out_count);

/**
 * @brief Takes ownership of the list of changed processor names.
 *
 * The caller is responsible for freeing the returned array of strings.
 *
 * @param change Pointer to the config_change_t instance.
 * @param out_count Pointer to store the number of processor names returned.
 * @return Array of string pointers containing the changed processor names.
 */
char** config_change_take_processors(config_change_t* change,
                                     size_t* out_count);

#endif  // CLIB_CONFIG_CONFIG_DIFF_H
