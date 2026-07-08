/**
 * @file config_loader.h
 * @brief JSON loader for DSPConfiguration.
 *
 * All configuration validation logic (top-level schema bounds, per-component
 * constraints, and pipeline channel verification) resides inside the model
 * definitions in the `DSPConfig` package. This file is responsible only
 * for decoding the JSON representation of the configuration.
 *
 * DSPMonitor only ever sends JSON over the actor's
 * `start(configJson:)` boundary, so the loader is JSON-only; the
 * YAML pathway and Yams dependency have been pruned.
 */

#ifndef CLIB_PIPELINE_CONFIG_LOADER_H
#define CLIB_PIPELINE_CONFIG_LOADER_H

#include "Config/config_error.h"
#include "Config/configuration.h"

/**
 * @brief Parse a DSP configuration from JSON and run full validation.
 *
 * @param[in] json The JSON string representing the DSP configuration.
 * @param[out] out_config Pointer to a pointer to receive the parsed
 * configuration. Must be freed by the caller.
 * @param[out] err Pointer to a config error struct to receive error details on
 * failure.
 * @return 0 on success, non-zero on failure.
 */
int config_loader_parse(const char* json, dsp_config_t** out_config,
                        config_error_t* err);

#endif  // CLIB_PIPELINE_CONFIG_LOADER_H
