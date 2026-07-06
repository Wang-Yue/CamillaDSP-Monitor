// JSON loader for `DSPConfiguration`.
//
// All configuration validation logic (top-level schema bounds, per-component
// constraints, and pipeline channel verification) resides inside the model
// definitions in the `DSPConfig` package. This file is responsible only
// for decoding the JSON representation of the configuration.
//
// DSPMonitor only ever sends JSON over the actor's
// `start(configJson:)` boundary, so the loader is JSON-only; the
// YAML pathway and Yams dependency have been pruned.

#ifndef CLIB_PIPELINE_CONFIG_LOADER_H
#define CLIB_PIPELINE_CONFIG_LOADER_H

#include "Config/configuration.h"
#include "Config/config_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Parse a DSP configuration from JSON and run full validation.
int config_loader_parse(const char* json, dsp_config_t** out_config, config_error_t* err);

#ifdef __cplusplus
}
#endif

#endif // CLIB_PIPELINE_CONFIG_LOADER_H
