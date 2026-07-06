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

#include "Pipeline/config_loader.h"

/// Parse a DSP configuration from JSON and run full validation.
int config_loader_parse(const char* json, dsp_config_t** out_config,
                        config_error_t* err) {
  return dsp_config_parse_json(json, out_config, err);
}
