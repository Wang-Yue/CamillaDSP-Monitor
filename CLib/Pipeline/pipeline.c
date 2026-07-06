#include "Pipeline/pipeline.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static bool string_list_contains(const char* const* list, size_t count, const char* name) {
    if (!list || !name) return false;
    for (size_t i = 0; i < count; i++) {
        if (list[i] && strcmp(list[i], name) == 0) return true;
    }
    return false;
}

/// Destroy and free the pipeline.
void pipeline_free(pipeline_t* pipeline) {
    if (!pipeline) return;
    if (pipeline->master_volume) {
        volume_filter_free(pipeline->master_volume);
    }
    if (pipeline->capture_scratch) {
        audio_chunk_free(pipeline->capture_scratch);
    }
    if (pipeline->scratches_for_mixers) {
        for (size_t i = 0; i < pipeline->scratches_for_mixers_count; i++) {
            if (pipeline->scratches_for_mixers[i]) {
                audio_chunk_free(pipeline->scratches_for_mixers[i]);
            }
        }
        free(pipeline->scratches_for_mixers);
    }
    if (pipeline->steps) {
        for (size_t i = 0; i < pipeline->steps_count; i++) {
            pipeline_exec_step_t* step = &pipeline->steps[i];
            if (step->type == EXEC_STEP_FILTER) {
                if (step->filters) {
                    for (size_t j = 0; j < step->filters_count; j++) {
                        if (step->filters[j]) {
                            filter_free(step->filters[j]);
                        }
                    }
                    free(step->filters);
                }
            } else if (step->type == EXEC_STEP_MIXER) {
                if (step->mixer) {
                    audio_mixer_free(step->mixer);
                }
            } else if (step->type == EXEC_STEP_PROCESSOR) {
                if (step->processor) {
                    dsp_processor_free(step->processor);
                }
            }
        }
        free(pipeline->steps);
    }
    free(pipeline);
}

/// Initialize the main audio processing pipeline.
pipeline_t* pipeline_create(const dsp_config_t* config, processing_parameters_t* proc_params, size_t explicit_chunk_size, config_error_t* err) {
    if (!config) {
        config_error_set(err, CONFIG_ERR_VALIDATION, "Configuration is NULL");
        return NULL;
    }

    pipeline_t* pipeline = (pipeline_t*)calloc(1, sizeof(pipeline_t));
    if (!pipeline) {
        config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
        return NULL;
    }

    pipeline->frames_per_chunk = explicit_chunk_size > 0 ? explicit_chunk_size : config->devices.chunksize;
    pipeline->rate = config->devices.samplerate;
    pipeline->expected_in_channels = config->devices.capture.channels;

    // Create the implicit master volume filter — equivalent to the
    // master volume slot (which keys off
    // fader index 0). Reads its initial state from the shared
    // `processingParameters` so the engine's pre-start
    // `setVolume`/`setMute` calls are honoured without a 0 dB ramp.
    // Read the volume ramp time and safety limits from the devices configuration.
    volume_parameters_t vol_params;
    memset(&vol_params, 0, sizeof(vol_params));
    vol_params.ramp_time = config->devices.has_volume_ramp_time ? config->devices.volume_ramp_time : 400.0;
    vol_params.has_ramp_time = true;
    vol_params.limit = config->devices.has_volume_limit ? config->devices.volume_limit : 50.0;
    vol_params.has_limit = true;
    vol_params.fader = FADER_MAIN;

    pipeline->master_volume = volume_filter_create("master_volume", &vol_params, pipeline->rate, pipeline->frames_per_chunk, proc_params);
    if (!pipeline->master_volume) {
        config_error_set(err, CONFIG_ERR_PARSE, "Failed to create master volume filter");
        pipeline_free(pipeline);
        return NULL;
    }

    // Pre-allocate the input scratch sized for the capture-side channel count.
    pipeline->capture_scratch = audio_chunk_create(pipeline->frames_per_chunk, pipeline->expected_in_channels);
    if (!pipeline->capture_scratch) {
        config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate capture scratch buffer");
        pipeline_free(pipeline);
        return NULL;
    }

    size_t total_exec_steps = 0;
    size_t num_mixers = 0;
    // Track current channel count as we walk pipeline steps
    size_t current_channels = pipeline->expected_in_channels;

    if (config->pipeline && config->pipeline_count > 0) {
        for (size_t i = 0; i < config->pipeline_count; i++) {
            const pipeline_step_t* step = &config->pipeline[i];
            if (step->type == PIPELINE_STEP_TYPE_FILTER) {
                if (step->channels && step->channels_count > 0) {
                    total_exec_steps += step->channels_count;
                } else if (step->has_channel) {
                    total_exec_steps += 1;
                } else {
                    total_exec_steps += current_channels;
                }
            } else if (step->type == PIPELINE_STEP_TYPE_MIXER) {
                total_exec_steps += 1;
                num_mixers += 1;
                const mixer_config_t* m_cfg = dsp_config_get_mixer(config, step->name);
                if (m_cfg) {
                    current_channels = m_cfg->channels_out;
                }
            } else if (step->type == PIPELINE_STEP_TYPE_PROCESSOR) {
                total_exec_steps += 1;
            }
        }
    }

    if (total_exec_steps > 0) {
        pipeline->steps = (pipeline_exec_step_t*)calloc(total_exec_steps, sizeof(pipeline_exec_step_t));
        if (!pipeline->steps) {
            config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
            pipeline_free(pipeline);
            return NULL;
        }
        pipeline->steps_count = total_exec_steps;
    }
    if (num_mixers > 0) {
        pipeline->scratches_for_mixers = (audio_chunk_t**)calloc(num_mixers, sizeof(audio_chunk_t*));
        if (!pipeline->scratches_for_mixers) {
            config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
            pipeline_free(pipeline);
            return NULL;
        }
        pipeline->scratches_for_mixers_count = num_mixers;
    }

    current_channels = pipeline->expected_in_channels;
    size_t exec_idx = 0;
    size_t mixer_idx = 0;

    if (config->pipeline && config->pipeline_count > 0) {
        for (size_t i = 0; i < config->pipeline_count; i++) {
            const pipeline_step_t* step = &config->pipeline[i];
            switch (step->type) {
                case PIPELINE_STEP_TYPE_FILTER: {
                    if (!step->names || step->names_count == 0) {
                        config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Filter step missing names");
                        pipeline_free(pipeline);
                        return NULL;
                    }
                    bool is_bypassed = step->bypassed;
                    int* channels_to_apply = NULL;
                    size_t channels_count = 0;
                    int single_ch = 0;
                    int* all_chs = NULL;

                    if (step->channels && step->channels_count > 0) {
                        channels_to_apply = step->channels;
                        channels_count = step->channels_count;
                    } else if (step->has_channel) {
                        single_ch = step->channel;
                        channels_to_apply = &single_ch;
                        channels_count = 1;
                    } else {
                        all_chs = (int*)malloc(current_channels * sizeof(int));
                        if (!all_chs) {
                            config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
                            pipeline_free(pipeline);
                            return NULL;
                        }
                        for (size_t c = 0; c < current_channels; c++) all_chs[c] = (int)c;
                        channels_to_apply = all_chs;
                        channels_count = current_channels;
                    }

                    // Create a separate filter chain for each target channel
                    for (size_t c = 0; c < channels_count; c++) {
                        int ch = channels_to_apply[c];
                        pipeline_exec_step_t* exec = &pipeline->steps[exec_idx++];
                        exec->type = EXEC_STEP_FILTER;
                        exec->bypassed = is_bypassed;
                        exec->channel = ch;
                        exec->filters_count = step->names_count;
                        exec->filters = (filter_t**)calloc(step->names_count, sizeof(filter_t*));
                        if (!exec->filters) {
                            if (all_chs) free(all_chs);
                            config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
                            pipeline_free(pipeline);
                            return NULL;
                        }

                        for (size_t j = 0; j < step->names_count; j++) {
                            const filter_config_t* f_cfg = dsp_config_get_filter(config, step->names[j]);
                            if (!f_cfg) {
                                if (all_chs) free(all_chs);
                                config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Filter '%s' not defined", step->names[j]);
                                pipeline_free(pipeline);
                                return NULL;
                            }
                            filter_t* f = filter_create(step->names[j], f_cfg, pipeline->rate, pipeline->frames_per_chunk, proc_params);
                            if (!f) {
                                if (all_chs) free(all_chs);
                                config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Failed to create filter '%s'", step->names[j]);
                                pipeline_free(pipeline);
                                return NULL;
                            }
                            exec->filters[j] = f;
                        }
                    }
                    if (all_chs) free(all_chs);
                    break;
                }
                case PIPELINE_STEP_TYPE_MIXER: {
                    if (!step->has_name || step->name[0] == '\0') {
                        config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Mixer step missing name or config");
                        pipeline_free(pipeline);
                        return NULL;
                    }
                    const mixer_config_t* m_cfg = dsp_config_get_mixer(config, step->name);
                    if (!m_cfg) {
                        config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Mixer step missing name or config");
                        pipeline_free(pipeline);
                        return NULL;
                    }
                    audio_mixer_t* m = audio_mixer_create(step->name, m_cfg, pipeline->frames_per_chunk);
                    if (!m) {
                        config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Failed to create mixer '%s'", step->name);
                        pipeline_free(pipeline);
                        return NULL;
                    }
                    current_channels = m_cfg->channels_out;
                    audio_chunk_t* scratch = audio_chunk_create(pipeline->frames_per_chunk, current_channels);
                    if (!scratch) {
                        audio_mixer_free(m);
                        config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate mixer scratch buffer");
                        pipeline_free(pipeline);
                        return NULL;
                    }
                    pipeline->scratches_for_mixers[mixer_idx++] = scratch;

                    pipeline_exec_step_t* exec = &pipeline->steps[exec_idx++];
                    exec->type = EXEC_STEP_MIXER;
                    exec->bypassed = step->bypassed;
                    exec->mixer = m;
                    break;
                }
                case PIPELINE_STEP_TYPE_PROCESSOR: {
                    if (!step->has_name || step->name[0] == '\0') {
                        config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Processor step missing name or config");
                        pipeline_free(pipeline);
                        return NULL;
                    }
                    const processor_config_t* p_cfg = dsp_config_get_processor(config, step->name);
                    if (!p_cfg) {
                        config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Processor step missing name or config");
                        pipeline_free(pipeline);
                        return NULL;
                    }
                    dsp_processor_t* p = dsp_processor_create(step->name, p_cfg, pipeline->rate, pipeline->frames_per_chunk);
                    if (!p) {
                        config_error_set(err, CONFIG_ERR_INVALID_PIPELINE, "Failed to create processor '%s'", step->name);
                        pipeline_free(pipeline);
                        return NULL;
                    }
                    pipeline_exec_step_t* exec = &pipeline->steps[exec_idx++];
                    exec->type = EXEC_STEP_PROCESSOR;
                    exec->bypassed = step->bypassed;
                    exec->processor = p;
                    break;
                }
            }
        }
    }

    pipeline->steps_count = exec_idx;
    pipeline->expected_out_channels = current_channels;
    return pipeline;
}

/// Process an input audio chunk into an output audio chunk.
pipeline_error_t pipeline_process(pipeline_t* pipeline, const audio_chunk_t* input, audio_chunk_t* output) {
    if (!pipeline || !input || !output) return PIPELINE_ERR_INPUT_SIZE_MISMATCH;
    size_t valid_frames = input->valid_frames;

    // 1. Validate input and output buffer shapes/capacities against pipeline configurations.
    if (valid_frames > pipeline->frames_per_chunk) {
        pipeline->last_error_needed = pipeline->frames_per_chunk;
        pipeline->last_error_got = valid_frames;
        return PIPELINE_ERR_INPUT_SIZE_MISMATCH;
    }
    if (audio_chunk_get_channels(input) != pipeline->expected_in_channels) {
        pipeline->last_error_needed = pipeline->expected_in_channels;
        pipeline->last_error_got = audio_chunk_get_channels(input);
        return PIPELINE_ERR_CHANNEL_COUNT_MISMATCH;
    }
    if (audio_chunk_get_channels(output) != pipeline->expected_out_channels) {
        pipeline->last_error_needed = pipeline->expected_out_channels;
        pipeline->last_error_got = audio_chunk_get_channels(output);
        return PIPELINE_ERR_CHANNEL_COUNT_MISMATCH;
    }
    if (audio_chunk_get_frames(output) < valid_frames) {
        pipeline->last_error_needed = valid_frames;
        pipeline->last_error_got = audio_chunk_get_frames(output);
        return PIPELINE_ERR_OUTPUT_BUFFER_TOO_SMALL;
    }

    // 2. Copy input into our pre-allocated scratch. The class-backed
    // `AudioBuffers` no longer shields the caller's chunk from in-place
    // mutation, so we make our own working copy up front.
    for (size_t ch = 0; ch < pipeline->expected_in_channels; ch++) {
        waveform_t src = audio_chunk_get_channel(input, ch);
        mutable_waveform_t dst = audio_chunk_get_channel(pipeline->capture_scratch, ch);
        if (src && dst && valid_frames > 0) {
            memcpy(dst, src, valid_frames * sizeof(double));
        }
    }
    pipeline->capture_scratch->valid_frames = valid_frames;

    audio_chunk_t* current_chunk = pipeline->capture_scratch;
    // 3. Implicit main volume with smooth ramp.
    // Mutates workingChunk's samples in place.
    volume_filter_prepare_chunk(pipeline->master_volume);
    for (size_t ch = 0; ch < audio_chunk_get_channels(current_chunk); ch++) {
        mutable_waveform_t buf = audio_chunk_get_channel(current_chunk, ch);
        if (buf && valid_frames > 0) {
            volume_filter_process(pipeline->master_volume, buf, valid_frames);
        }
    }
    volume_filter_advance_ramp(pipeline->master_volume);

    // 4. Execute pipeline steps sequentially.
    size_t mixer_idx = 0;
    for (size_t i = 0; i < pipeline->steps_count; i++) {
        pipeline_exec_step_t* step = &pipeline->steps[i];
        switch (step->type) {
            case EXEC_STEP_FILTER: {
                if (step->bypassed) continue;
                if ((size_t)step->channel >= audio_chunk_get_channels(current_chunk)) continue;
                mutable_waveform_t buf = audio_chunk_get_channel(current_chunk, step->channel);
                for (size_t j = 0; j < step->filters_count; j++) {
                    if (step->filters[j] && valid_frames > 0) {
                        filter_process(step->filters[j], buf, valid_frames);
                    }
                }
                break;
            }
            case EXEC_STEP_MIXER: {
                if (mixer_idx >= pipeline->scratches_for_mixers_count) continue;
                audio_chunk_t* scratch = pipeline->scratches_for_mixers[mixer_idx];
                mixer_error_t err = audio_mixer_process(step->mixer, current_chunk, scratch);
                if (err != MIXER_OK) {
                    if (err == MIXER_ERR_INPUT_SIZE_MISMATCH) {
                        pipeline->last_error_needed = pipeline->frames_per_chunk;
                        pipeline->last_error_got = valid_frames;
                        return PIPELINE_ERR_INPUT_SIZE_MISMATCH;
                    }
                    if (err == MIXER_ERR_OUTPUT_BUFFER_TOO_SMALL) {
                        pipeline->last_error_needed = valid_frames;
                        pipeline->last_error_got = audio_chunk_get_frames(scratch);
                        return PIPELINE_ERR_OUTPUT_BUFFER_TOO_SMALL;
                    }
                    pipeline->last_error_needed = step->mixer->channels_in;
                    pipeline->last_error_got = audio_chunk_get_channels(current_chunk);
                    return PIPELINE_ERR_CHANNEL_COUNT_MISMATCH;
                }
                current_chunk = scratch;
                mixer_idx++;
                break;
            }
            case EXEC_STEP_PROCESSOR: {
                if (step->bypassed) continue;
                if (step->processor) {
                    dsp_processor_process(step->processor, current_chunk);
                }
                break;
            }
        }
    }

    // 5. Copy the final computed samples from workingChunk to caller-supplied output buffer.
    output->valid_frames = valid_frames;
    for (size_t ch = 0; ch < pipeline->expected_out_channels; ch++) {
        if (ch >= audio_chunk_get_channels(current_chunk)) break;
        waveform_t src = audio_chunk_get_channel(current_chunk, ch);
        mutable_waveform_t dst = audio_chunk_get_channel(output, ch);
        if (src && dst && valid_frames > 0) {
            memcpy(dst, src, valid_frames * sizeof(double));
        }
    }
    return PIPELINE_OK;
}

/// Update parameters for filters, mixers, and processors in the pipeline.
void pipeline_update_parameters(
    pipeline_t* pipeline,
    const dsp_config_t* config,
    const char* const* filters, size_t filters_count,
    const char* const* mixers, size_t mixers_count,
    const char* const* processors, size_t processors_count
) {
    if (!pipeline || !config) return;
    for (size_t i = 0; i < pipeline->steps_count; i++) {
        pipeline_exec_step_t* step = &pipeline->steps[i];
        switch (step->type) {
            case EXEC_STEP_FILTER:
                for (size_t j = 0; j < step->filters_count; j++) {
                    filter_t* f = step->filters[j];
                    if (f && string_list_contains(filters, filters_count, f->name)) {
                        filter_config_t* f_cfg = dsp_config_get_filter(config, f->name);
                        if (f_cfg) {
                            filter_update_parameters(f, f_cfg, pipeline->rate);
                        }
                    }
                }
                break;
            case EXEC_STEP_MIXER:
                if (step->mixer && step->mixer->name && string_list_contains(mixers, mixers_count, step->mixer->name)) {
                    mixer_config_t* m_cfg = dsp_config_get_mixer(config, step->mixer->name);
                    if (m_cfg) {
                        audio_mixer_update_parameters(step->mixer, m_cfg);
                    }
                }
                break;
            case EXEC_STEP_PROCESSOR: {
                const char* p_name = dsp_processor_get_name(step->processor);
                if (step->processor && p_name && string_list_contains(processors, processors_count, p_name)) {
                    processor_config_t* p_cfg = dsp_config_get_processor(config, p_name);
                    if (p_cfg) {
                        dsp_processor_update_parameters(step->processor, p_cfg, pipeline->rate);
                    }
                }
                break;
            }
        }
    }
}
