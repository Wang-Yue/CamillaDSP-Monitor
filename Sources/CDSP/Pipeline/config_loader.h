// JSON loader and cross-component validator for `DSPConfiguration`.
//
// Per-domain validation (filter parameters, mixer mappings) lives next
// to the validated types — see `BiquadParameters.validate(sampleRate:)`,
// `GainParameters.validate()`, `LoudnessParameters.validate()`, and
// `MixerConfig.validate()`. This file owns only:
//   1. JSON → `DSPConfiguration` decoding.
//   2. Top-level field checks (samplerate, chunksize, channel counts).
//   3. The pipeline walk that ties filters/mixers to the device channel
//      counts.
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

/// Validate a parsed configuration. Top-level field checks first,
/// then per-component validation, then the pipeline walk.
///
/// Walk the pipeline tracking the channel count through each step.
/// Mirrors the logic in the Rust `config::utils::validate_config`
/// pipeline walk:
///   - Filter step: all channel indices must be < current count;
///     count is unchanged.
///   - Mixer step: `channelsIn` must match current count; count
///     becomes `channelsOut`.
/// After the walk, the count must equal the playback channel count.
int config_loader_validate(const dsp_config_t* config, config_error_t* err);

#ifdef __cplusplus
}
#endif

#endif // CLIB_PIPELINE_CONFIG_LOADER_H
