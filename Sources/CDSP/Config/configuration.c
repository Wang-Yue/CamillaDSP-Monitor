#include "configuration.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

// Top-level configuration data structures and validation logic. The JSON loader
// lives in `config_loader.c`.
//
// This file owns:
//   1. Top-level configuration models (dsp_config_t and pipeline_step_t).
//   2. Cross-component validation logic, including schema checks and the
//      pipeline walk that tracks channel layouts.

filter_config_t* dsp_config_get_filter(const dsp_config_t* config, const char* name) {
    if (!config || !name) return NULL;
    for (size_t i = 0; i < config->filters_count; i++) {
        if (strcmp(config->filters[i].name, name) == 0) {
            return &config->filters[i].filter;
        }
    }
    return NULL;
}

mixer_config_t* dsp_config_get_mixer(const dsp_config_t* config, const char* name) {
    if (!config || !name) return NULL;
    for (size_t i = 0; i < config->mixers_count; i++) {
        if (strcmp(config->mixers[i].name, name) == 0) {
            return &config->mixers[i].mixer;
        }
    }
    return NULL;
}

processor_config_t* dsp_config_get_processor(const dsp_config_t* config, const char* name) {
    if (!config || !name) return NULL;
    for (size_t i = 0; i < config->processors_count; i++) {
        if (strcmp(config->processors[i].name, name) == 0) {
            return &config->processors[i].processor;
        }
    }
    return NULL;
}

/// Top-level configuration consumed by the DSP engine.
/// One step in the user-defined processing pipeline. Either a named
/// filter chain applied to one or more channels, or a mixer that
/// changes the channel layout.
int dsp_config_validate(const dsp_config_t* config, config_error_t* err) {
    if (!config) return 0;
    
    // Top level checks
    if (config->devices.samplerate <= 0) {
        config_error_set(err, CONFIG_ERR_VALIDATION, "Sample rate must be positive");
        return -1;
    }
    if (config->devices.chunksize <= 0) {
        config_error_set(err, CONFIG_ERR_VALIDATION, "Chunk size must be positive");
        return -1;
    }
    if (config->devices.capture.channels <= 0) {
        config_error_set(err, CONFIG_ERR_VALIDATION, "Capture channels must be positive");
        return -1;
    }
    if (config->devices.playback.channels <= 0) {
        config_error_set(err, CONFIG_ERR_VALIDATION, "Playback channels must be positive");
        return -1;
    }
    
    // Validate filters
    for (size_t i = 0; i < config->filters_count; i++) {
        config_error_t sub_err;
        config_error_init(&sub_err);
        if (filter_config_validate(&config->filters[i].filter, config->devices.samplerate, &sub_err) != 0) {
            config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Filter '%s': %s", config->filters[i].name, sub_err.message);
            return -1;
        }
    }
    
    // Validate mixers
    for (size_t i = 0; i < config->mixers_count; i++) {
        config_error_t sub_err;
        config_error_init(&sub_err);
        if (mixer_config_validate(&config->mixers[i].mixer, &sub_err) != 0) {
            config_error_set(err, CONFIG_ERR_INVALID_MIXER, "Mixer '%s': %s", config->mixers[i].name, sub_err.message);
            return -1;
        }
    }
    
    // Validate processors
    for (size_t i = 0; i < config->processors_count; i++) {
        config_error_t sub_err;
        config_error_init(&sub_err);
        if (processor_config_validate(&config->processors[i].processor, &sub_err) != 0) {
            config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Processor '%s': %s", config->processors[i].name, sub_err.message);
            return -1;
        }
    }
    
    // Validate pipeline
    int num_channels = config->devices.capture.channels;
    for (size_t i = 0; i < config->pipeline_count; i++) {
        const pipeline_step_t* step = &config->pipeline[i];
        if (step->bypassed) continue;
        
        switch (step->type) {
            case PIPELINE_STEP_TYPE_FILTER: {
                if (!step->names || step->names_count == 0) {
                    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Filter step %zu must have 'names'", i);
                    return -1;
                }
                if (!step->has_channel && (!step->channels || step->channels_count == 0)) {
                    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Filter step %zu must have 'channel' or 'channels'", i);
                    return -1;
                }
                for (size_t j = 0; j < step->names_count; j++) {
                    if (!dsp_config_get_filter(config, step->names[j])) {
                        config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Filter '%s' referenced in pipeline but not defined", step->names[j]);
                        return -1;
                    }
                }
                if (step->has_channel) {
                    if (step->channel >= num_channels) {
                        config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Filter step %zu references channel %d but pipeline only has %d channel(s) at this point", i, step->channel, num_channels);
                        return -1;
                    }
                }
                for (size_t j = 0; j < step->channels_count; j++) {
                    if (step->channels[j] >= num_channels) {
                        config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Filter step %zu references channel %d but pipeline only has %d channel(s) at this point", i, step->channels[j], num_channels);
                        return -1;
                    }
                }
                break;
            }
            case PIPELINE_STEP_TYPE_MIXER: {
                if (!step->has_name || step->name[0] == '\0') {
                    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Mixer step %zu must have 'name'", i);
                    return -1;
                }
                const mixer_config_t* mixer = dsp_config_get_mixer(config, step->name);
                if (!mixer) {
                    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Mixer '%s' referenced in pipeline but not defined", step->name);
                    return -1;
                }
                if (mixer->channels_in != (size_t)num_channels) {
                    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Mixer '%s' expects %d input channel(s) but pipeline has %d at this point", step->name, mixer->channels_in, num_channels);
                    return -1;
                }
                num_channels = mixer->channels_out;
                break;
            }
            case PIPELINE_STEP_TYPE_PROCESSOR: {
                if (!step->has_name || step->name[0] == '\0') {
                    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Processor step %zu must have 'name'", i);
                    return -1;
                }
                const processor_config_t* proc = dsp_config_get_processor(config, step->name);
                if (!proc) {
                    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Processor '%s' referenced in pipeline but not defined", step->name);
                    return -1;
                }
                int expected_channels = 0;
                switch (proc->type) {
                    case PROCESSOR_TYPE_COMPRESSOR: expected_channels = proc->parameters.compressor.channels; break;
                    case PROCESSOR_TYPE_NOISE_GATE: expected_channels = proc->parameters.noise_gate.channels; break;
                    case PROCESSOR_TYPE_RACE: expected_channels = proc->parameters.race.channels; break;
                }
                if (expected_channels != num_channels) {
                    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Processor '%s' expects %d channel(s) but pipeline has %d at this point", step->name, expected_channels, num_channels);
                    return -1;
                }
                break;
            }
        }
    }
    
    int playback_channels = config->devices.playback.channels;
    if (num_channels != playback_channels) {
        config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Pipeline outputs %d channel(s) but playback device expects %d", num_channels, playback_channels);
        return -1;
    }
    
    return 0;
}

static const char* find_section_end(const char* start) {
    if (!start) return NULL;
    const char* p = strchr(start, '{');
    if (!p) return start + strlen(start);
    int depth = 0;
    bool in_quote = false;
    bool escape = false;
    for (; *p; p++) {
        if (in_quote) {
            if (escape) escape = false;
            else if (*p == '\\') escape = true;
            else if (*p == '"') in_quote = false;
        } else {
            if (*p == '"') in_quote = true;
            else if (*p == '{') depth++;
            else if (*p == '}') {
                depth--;
                if (depth == 0) return p + 1;
            }
        }
    }
    return start + strlen(start);
}

static int extract_int_in_range(const char* start, const char* end, const char* key, int default_val) {
    if (!start || !end || !key) return default_val;
    const char* pos = strstr(start, key);
    if (!pos || pos >= end) return default_val;
    pos += strlen(key);
    while (pos < end && (*pos == ' ' || *pos == ':' || *pos == '\t' || *pos == '\n' || *pos == '\r')) pos++;
    if (pos >= end || (!isdigit((unsigned char)*pos) && *pos != '-')) return default_val;
    return atoi(pos);
}

static bool extract_string_in_range(const char* start, const char* end, const char* key, char* out_buf, size_t max_len) {
    if (!start || !end || !key || !out_buf || max_len == 0) return false;
    const char* pos = strstr(start, key);
    if (!pos || pos >= end) return false;
    pos += strlen(key);
    while (pos < end && (*pos == ' ' || *pos == ':' || *pos == '\t' || *pos == '\n' || *pos == '\r')) pos++;
    if (pos >= end || *pos != '"') return false;
    pos++;
    size_t i = 0;
    while (pos < end && *pos && *pos != '"' && i < max_len - 1) {
        if (*pos == '\\' && pos + 1 < end && *(pos + 1)) pos++;
        out_buf[i++] = *pos++;
    }
    out_buf[i] = '\0';
    return true;
}

static bool extract_bool_in_range(const char* start, const char* end, const char* key, bool default_val) {
    if (!start || !end || !key) return default_val;
    const char* pos = strstr(start, key);
    if (!pos || pos >= end) return default_val;
    pos += strlen(key);
    while (pos < end && (*pos == ' ' || *pos == ':' || *pos == '\t' || *pos == '\n' || *pos == '\r')) pos++;
    if (pos + 4 <= end && strncmp(pos, "true", 4) == 0) return true;
    if (pos + 5 <= end && strncmp(pos, "false", 5) == 0) return false;
    return default_val;
}

static double extract_double_in_range(const char* start, const char* end, const char* key, double default_val) {
    if (!start || !end || !key) return default_val;
    const char* pos = strstr(start, key);
    if (!pos || pos >= end) return default_val;
    pos += strlen(key);
    while (pos < end && (*pos == ' ' || *pos == ':' || *pos == '\t' || *pos == '\n' || *pos == '\r')) pos++;
    if (pos >= end || (!isdigit((unsigned char)*pos) && *pos != '-' && *pos != '.')) return default_val;
    return atof(pos);
}

int dsp_config_parse_json(const char* json, dsp_config_t** out_config, config_error_t* err) {
    if (!json || !out_config) {
        config_error_set(err, CONFIG_ERR_PARSE, "JSON string or output pointer is NULL");
        return -1;
    }
    
    // Check syntax (simple brace matching and quote state)
    int brace_depth = 0;
    bool in_quote = false;
    bool escape = false;
    for (const char* p = json; *p; p++) {
        if (in_quote) {
            if (escape) {
                escape = false;
            } else if (*p == '\\') {
                escape = true;
            } else if (*p == '"') {
                in_quote = false;
            }
        } else {
            if (*p == '"') {
                in_quote = true;
            } else if (*p == '{') {
                brace_depth++;
            } else if (*p == '}') {
                brace_depth--;
                if (brace_depth < 0) {
                    config_error_set(err, CONFIG_ERR_PARSE, "Unmatched closing brace in JSON");
                    return -1;
                }
            }
        }
    }
    if (brace_depth != 0 || in_quote) {
        config_error_set(err, CONFIG_ERR_PARSE, "JSON syntax error: unmatched braces or quotes");
        return -1;
    }
    
    dsp_config_t* config = (dsp_config_t*)calloc(1, sizeof(dsp_config_t));
    if (!config) {
        config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
        return -1;
    }
    
    const char* json_end = json + strlen(json);
    config->devices.samplerate = extract_int_in_range(json, json_end, "\"samplerate\"", 0);
    config->devices.chunksize = extract_int_in_range(json, json_end, "\"chunksize\"", 0);
    config->devices.queuelimit = extract_int_in_range(json, json_end, "\"queuelimit\"", 0);
    config->devices.has_queuelimit = (config->devices.queuelimit > 0);
    config->devices.enable_rate_adjust = extract_bool_in_range(json, json_end, "\"enable_rate_adjust\"", false);
    config->devices.has_enable_rate_adjust = true;
    config->devices.target_level = extract_int_in_range(json, json_end, "\"target_level\"", 0);
    config->devices.has_target_level = (config->devices.target_level > 0);
    config->devices.adjust_period = extract_double_in_range(json, json_end, "\"adjust_period\"", 10.0);
    config->devices.has_adjust_period = (config->devices.adjust_period > 0);
    config->devices.silence_threshold = extract_double_in_range(json, json_end, "\"silence_threshold\"", 0.0);
    config->devices.has_silence_threshold = (config->devices.silence_threshold != 0.0);
    config->devices.silence_timeout = extract_double_in_range(json, json_end, "\"silence_timeout\"", 0.0);
    config->devices.has_silence_timeout = (config->devices.silence_timeout > 0.0);
    config->devices.capture_samplerate = extract_int_in_range(json, json_end, "\"capture_samplerate\"", 0);
    config->devices.has_capture_samplerate = (config->devices.capture_samplerate > 0);
    config->devices.volume_ramp_time = extract_double_in_range(json, json_end, "\"volume_ramp_time\"", 0.0);
    config->devices.has_volume_ramp_time = (config->devices.volume_ramp_time > 0.0);
    config->devices.volume_limit = extract_double_in_range(json, json_end, "\"volume_limit\"", 0.0);
    config->devices.has_volume_limit = (config->devices.volume_limit > 0.0);
    config->devices.stop_on_rate_change = extract_bool_in_range(json, json_end, "\"stop_on_rate_change\"", false);
    config->devices.has_stop_on_rate_change = true;
    config->devices.rate_measure_interval = extract_double_in_range(json, json_end, "\"rate_measure_interval\"", 0.0);
    config->devices.has_rate_measure_interval = (config->devices.rate_measure_interval > 0.0);
    config->devices.multithreaded = extract_bool_in_range(json, json_end, "\"multithreaded\"", false);
    config->devices.has_multithreaded = true;
    config->devices.worker_threads = extract_int_in_range(json, json_end, "\"worker_threads\"", 0);
    config->devices.has_worker_threads = (config->devices.worker_threads > 0);

    const char* res_pos = strstr(json, "\"resampler\"");
    if (res_pos) {
        const char* res_end = find_section_end(res_pos);
        char type_str[64] = {0};
        if (extract_string_in_range(res_pos, res_end, "\"type\"", type_str, sizeof(type_str))) {
            config->devices.resampler.type = resampler_type_from_string(type_str);
        }
        if (extract_string_in_range(res_pos, res_end, "\"profile\"", config->devices.resampler.profile, sizeof(config->devices.resampler.profile))) {
            config->devices.resampler.has_profile = true;
        }
        if (extract_string_in_range(res_pos, res_end, "\"interpolation\"", config->devices.resampler.interpolation, sizeof(config->devices.resampler.interpolation))) {
            config->devices.resampler.has_interpolation = true;
        }
#if defined(__APPLE__)
        char aq_str[64] = {0};
        if (extract_string_in_range(res_pos, res_end, "\"apple_quality\"", aq_str, sizeof(aq_str))) {
            config->devices.resampler.apple_quality = apple_resampler_quality_from_string(aq_str);
            config->devices.resampler.has_apple_quality = true;
        }
        char ac_str[64] = {0};
        if (extract_string_in_range(res_pos, res_end, "\"apple_complexity\"", ac_str, sizeof(ac_str))) {
            config->devices.resampler.apple_complexity = apple_resampler_complexity_from_string(ac_str);
            config->devices.resampler.has_apple_complexity = true;
        }
#endif
        config->devices.resampler.sinc_len = extract_int_in_range(res_pos, res_end, "\"sinc_len\"", 0);
        config->devices.resampler.has_sinc_len = (config->devices.resampler.sinc_len > 0);
        config->devices.resampler.oversampling_factor = extract_int_in_range(res_pos, res_end, "\"oversampling_factor\"", 0);
        config->devices.resampler.has_oversampling_factor = (config->devices.resampler.oversampling_factor > 0);
        if (extract_string_in_range(res_pos, res_end, "\"window\"", config->devices.resampler.window, sizeof(config->devices.resampler.window))) {
            config->devices.resampler.has_window = true;
        }
        config->devices.resampler.f_cutoff = extract_double_in_range(res_pos, res_end, "\"f_cutoff\"", 0.0);
        config->devices.resampler.has_f_cutoff = (config->devices.resampler.f_cutoff > 0.0);
        
        config->devices.has_resampler = true;
    }
    
    const char* cap_pos = strstr(json, "\"capture\"");
    if (cap_pos) {
        const char* cap_end = find_section_end(cap_pos);
        config->devices.capture.channels = extract_int_in_range(cap_pos, cap_end, "\"channels\"", 0);
#if defined(__APPLE__)
        config->devices.capture.type = AUDIO_BACKEND_TYPE_CORE_AUDIO;
#elif defined(__linux__)
        config->devices.capture.type = AUDIO_BACKEND_TYPE_ALSA;
#elif defined(_WIN32)
        config->devices.capture.type = AUDIO_BACKEND_TYPE_WASAPI;
#endif
        if (extract_string_in_range(cap_pos, cap_end, "\"device\"", config->devices.capture.device, sizeof(config->devices.capture.device))) {
            config->devices.capture.has_device = true;
        }
        char fmt_str[64];
        if (extract_string_in_range(cap_pos, cap_end, "\"format\"", fmt_str, sizeof(fmt_str))) {
#if defined(__linux__)
            config->devices.capture.format = alsa_sample_format_from_string(fmt_str);
#else
            config->devices.capture.format = sample_format_from_string(fmt_str);
#endif
            config->devices.capture.has_format = true;
        }
#if defined(__linux__)
        config->devices.capture.stop_on_inactive = extract_bool_in_range(cap_pos, cap_end, "\"stop_on_inactive\"", false);
        config->devices.capture.has_stop_on_inactive = true;
        if (extract_string_in_range(cap_pos, cap_end, "\"link_volume_control\"", config->devices.capture.link_volume_control, sizeof(config->devices.capture.link_volume_control))) {
            config->devices.capture.has_link_volume_control = true;
        }
        if (extract_string_in_range(cap_pos, cap_end, "\"link_mute_control\"", config->devices.capture.link_mute_control, sizeof(config->devices.capture.link_mute_control))) {
            config->devices.capture.has_link_mute_control = true;
        }
#endif
        config->devices.capture.bypass_dop = extract_bool_in_range(cap_pos, cap_end, "\"bypass_dop\"", false);
        config->devices.capture.has_bypass_dop = true;
        config->devices.capture.dop_cutoff_hz = extract_double_in_range(cap_pos, cap_end, "\"dop_cutoff_hz\"", 20000.0);
        config->devices.capture.has_dop_cutoff_hz = true;
    }
    
    const char* play_pos = strstr(json, "\"playback\"");
    if (play_pos) {
        const char* play_end = find_section_end(play_pos);
        config->devices.playback.channels = extract_int_in_range(play_pos, play_end, "\"channels\"", 0);
#if defined(__APPLE__)
        config->devices.playback.type = AUDIO_BACKEND_TYPE_CORE_AUDIO;
#elif defined(__linux__)
        config->devices.playback.type = AUDIO_BACKEND_TYPE_ALSA;
#elif defined(_WIN32)
        config->devices.playback.type = AUDIO_BACKEND_TYPE_WASAPI;
#endif
        if (extract_string_in_range(play_pos, play_end, "\"device\"", config->devices.playback.device, sizeof(config->devices.playback.device))) {
            config->devices.playback.has_device = true;
        }
        char fmt_str[64];
        if (extract_string_in_range(play_pos, play_end, "\"format\"", fmt_str, sizeof(fmt_str))) {
#if defined(__linux__)
            config->devices.playback.format = alsa_sample_format_from_string(fmt_str);
#else
            config->devices.playback.format = sample_format_from_string(fmt_str);
#endif
            config->devices.playback.has_format = true;
        }
        config->devices.playback.exclusive = extract_bool_in_range(play_pos, play_end, "\"exclusive\"", false);
        config->devices.playback.has_exclusive = true;
        config->devices.playback.output_dop = extract_bool_in_range(play_pos, play_end, "\"output_dop\"", false);
        config->devices.playback.has_output_dop = true;
    }
    
    if (dsp_config_validate(config, err) != 0) {
        dsp_config_free(config);
        return -1;
    }
    
    *out_config = config;
    return 0;
}

void dsp_config_free(dsp_config_t* config) {
    if (!config) return;
    if (config->filters) {
        for (size_t i = 0; i < config->filters_count; i++) {
            if (config->filters[i].filter.type == FILTER_TYPE_CONV) {
                free(config->filters[i].filter.parameters.conv.values);
            } else if (config->filters[i].filter.type == FILTER_TYPE_BIQUAD_COMBO) {
                free(config->filters[i].filter.parameters.biquad_combo.gains);
            } else if (config->filters[i].filter.type == FILTER_TYPE_DIFF_EQ) {
                free(config->filters[i].filter.parameters.diff_eq.a);
                free(config->filters[i].filter.parameters.diff_eq.b);
            }
        }
        free(config->filters);
    }
    if (config->mixers) {
        for (size_t i = 0; i < config->mixers_count; i++) {
            if (config->mixers[i].mixer.mapping) {
                for (size_t j = 0; j < config->mixers[i].mixer.mapping_count; j++) {
                    free(config->mixers[i].mixer.mapping[j].sources);
                }
                free(config->mixers[i].mixer.mapping);
            }
        }
        free(config->mixers);
    }
    if (config->processors) {
        for (size_t i = 0; i < config->processors_count; i++) {
            if (config->processors[i].processor.type == PROCESSOR_TYPE_COMPRESSOR) {
                free(config->processors[i].processor.parameters.compressor.monitor_channels);
                free(config->processors[i].processor.parameters.compressor.process_channels);
            } else if (config->processors[i].processor.type == PROCESSOR_TYPE_NOISE_GATE) {
                free(config->processors[i].processor.parameters.noise_gate.monitor_channels);
                free(config->processors[i].processor.parameters.noise_gate.process_channels);
            }
        }
        free(config->processors);
    }
    if (config->pipeline) {
        for (size_t i = 0; i < config->pipeline_count; i++) {
            free(config->pipeline[i].channels);
            if (config->pipeline[i].names) {
                for (size_t j = 0; j < config->pipeline[i].names_count; j++) {
                    free(config->pipeline[i].names[j]);
                }
                free(config->pipeline[i].names);
            }
        }
        free(config->pipeline);
    }
    free(config);
}
