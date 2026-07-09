#include "Pipeline/pipeline.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "Audio/audio_buffers.h"
#include "Audio/double_helpers.h"
#include "Filters/filter.h"
#include "Filters/volume.h"
#include "Mixer/mixer.h"
#include "Processors/processor.h"

/// A single step in the processing pipeline
typedef enum {
  /// Filter chain applied to a single channel
  EXEC_STEP_FILTER = 0,
  /// Mixer that changes channel routing.
  EXEC_STEP_MIXER,
  /// Audio processor applied to the chunk in-place.
  EXEC_STEP_PROCESSOR
} exec_step_type_t;

/// A single step in the processing pipeline
typedef struct {
  exec_step_type_t type;
  bool bypassed;
  // For EXEC_STEP_FILTER:
  int channel;
  filter_t** filters;
  size_t filters_count;
  // For EXEC_STEP_MIXER:
  audio_mixer_t* mixer;
  // For EXEC_STEP_PROCESSOR:
  dsp_processor_t* processor;
} pipeline_exec_step_t;

/// The main audio processing pipeline.
struct pipeline_s {
  pipeline_exec_step_t* steps;
  size_t steps_count;
  /// Implicit main volume filter with smooth ramping
  volume_filter_t* master_volume;
  /// Working scratch the pipeline copies the caller's input into at the start
  /// of each `process(...)`. With class-owned `AudioBuffers`, we can no
  /// longer rely on CoW to isolate mutations from the caller's `input`
  /// chunk — so we copy explicitly into this pre-allocated buffer.
  audio_chunk_t* capture_scratch;
  /// Pre-allocated scratch chunks mapped by the sequential step index in
  /// `steps` array to prevent Copy-On-Write allocations on the hot path.
  audio_chunk_t** scratches_for_mixers;
  size_t scratches_for_mixers_count;

  size_t frames_per_chunk;
  int rate;
  size_t expected_in_channels;
  size_t expected_out_channels;

  // For test inspection on error:
  size_t last_error_needed;
  size_t last_error_got;
};

/**
 * @brief Helper function to check if a string list contains a specific name.
 *
 * @param list The list of strings to search.
 * @param count The number of elements in the list.
 * @param name The string to search for.
 * @return true if the name is found in the list, else false.
 */
static bool string_list_contains(const char* const* list, size_t count,
                                 const char* name) {
  if (!list || !name) return false;
  for (size_t i = 0; i < count; i++) {
    if (list[i] && strcmp(list[i], name) == 0) return true;
  }
  return false;
}

void pipeline_transfer_state(pipeline_t* dest, const pipeline_t* src) {
  if (!dest || !src) return;

  // 1. Transfer master volume state
  if (dest->master_volume && src->master_volume) {
    volume_filter_transfer_state(dest->master_volume, src->master_volume);
  }

  // 2. Transfer steps state
  for (size_t d = 0; d < dest->steps_count; d++) {
    pipeline_exec_step_t* dest_step = &dest->steps[d];

    if (dest_step->type == EXEC_STEP_FILTER) {
      // Find matching filter step by channel
      for (size_t s = 0; s < src->steps_count; s++) {
        const pipeline_exec_step_t* src_step = &src->steps[s];
        if (src_step->type == EXEC_STEP_FILTER && src_step->channel == dest_step->channel) {
          // Transfer individual filters by name matching
          for (size_t df = 0; df < dest_step->filters_count; df++) {
            filter_t* dest_f = dest_step->filters[df];
            const char* dest_name = filter_get_name(dest_f);
            for (size_t sf = 0; sf < src_step->filters_count; sf++) {
              filter_t* src_f = src_step->filters[sf];
              if (strcmp(filter_get_name(src_f), dest_name) == 0) {
                filter_transfer_state(dest_f, src_f);
                break;
              }
            }
          }
          break;
        }
      }
    } else if (dest_step->type == EXEC_STEP_PROCESSOR) {
      // Find matching processor step by name
      const char* dest_name = dsp_processor_get_name(dest_step->processor);
      for (size_t s = 0; s < src->steps_count; s++) {
        const pipeline_exec_step_t* src_step = &src->steps[s];
        if (src_step->type == EXEC_STEP_PROCESSOR) {
          const char* src_name = dsp_processor_get_name(src_step->processor);
          if (strcmp(src_name, dest_name) == 0) {
            dsp_processor_transfer_state(dest_step->processor, src_step->processor);
            break;
          }
        }
      }
    }
  }
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
pipeline_t* pipeline_create(const dsp_config_t* config,
                            processing_parameters_t* proc_params,
                            size_t explicit_chunk_size, config_error_t* err) {
  if (!config) {
    config_error_set(err, CONFIG_ERR_VALIDATION, "Configuration is NULL");
    return NULL;
  }

  pipeline_t* pipeline = (pipeline_t*)calloc(1, sizeof(pipeline_t));
  if (!pipeline) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return NULL;
  }

  pipeline->frames_per_chunk =
      explicit_chunk_size > 0 ? explicit_chunk_size : config->devices.chunksize;
  pipeline->rate = config->devices.samplerate;
  pipeline->expected_in_channels =
      capture_device_config_get_channels(&config->devices.capture);

  // Create the implicit master volume filter — equivalent to the
  // master volume slot (which keys off
  // fader index 0). Reads its initial state from the shared
  // `processingParameters` so the engine's pre-start
  // `setVolume`/`setMute` calls are honoured without a 0 dB ramp.
  // Read the volume ramp time and safety limits from the devices configuration.
  volume_parameters_t vol_params;
  memset(&vol_params, 0, sizeof(vol_params));
  vol_params.ramp_time = config->devices.has_volume_ramp_time
                             ? config->devices.volume_ramp_time
                             : 400.0;
  vol_params.has_ramp_time = true;
  vol_params.limit =
      config->devices.has_volume_limit ? config->devices.volume_limit : 50.0;
  vol_params.has_limit = true;
  vol_params.fader = FADER_MAIN;

  pipeline->master_volume =
      volume_filter_create("master_volume", &vol_params, pipeline->rate,
                           pipeline->frames_per_chunk, proc_params);
  if (!pipeline->master_volume) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to create master volume filter");
    pipeline_free(pipeline);
    return NULL;
  }

  // Pre-allocate the input scratch sized for the capture-side channel count.
  pipeline->capture_scratch = audio_chunk_create(
      pipeline->frames_per_chunk, pipeline->expected_in_channels);
  if (!pipeline->capture_scratch) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate capture scratch buffer");
    pipeline_free(pipeline);
    return NULL;
  }

  size_t total_exec_steps = 0;
  size_t num_mixers = 0;
  // Track current channel count as we walk pipeline steps. Channel count can
  // change after passing through a mixer.
  size_t current_channels = pipeline->expected_in_channels;

  // First pass: Calculate the exact number of execution steps and mixers
  // needed. This is required because a single config step (e.g. a filter
  // applied to multiple channels) may expand to multiple execution steps (one
  // per channel). Pre-allocating the execution steps array avoids dynamic
  // allocation during initialization of those steps.
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
    pipeline->steps = (pipeline_exec_step_t*)calloc(
        total_exec_steps, sizeof(pipeline_exec_step_t));
    if (!pipeline->steps) {
      config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
      pipeline_free(pipeline);
      return NULL;
    }
    pipeline->steps_count = total_exec_steps;
  }
  if (num_mixers > 0) {
    pipeline->scratches_for_mixers =
        (audio_chunk_t**)calloc(num_mixers, sizeof(audio_chunk_t*));
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
            config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                             "Filter step missing names");
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
              config_error_set(err, CONFIG_ERR_PARSE,
                               "Memory allocation failure");
              pipeline_free(pipeline);
              return NULL;
            }
            for (size_t c = 0; c < current_channels; c++) all_chs[c] = (int)c;
            channels_to_apply = all_chs;
            channels_count = current_channels;
          }

          // Create a separate filter chain for each target channel.
          // Each channel must have its own instance of filter state (e.g.,
          // history buffers for IIR/FIR filters) to avoid crosstalk and
          // incorrect filtering.
          for (size_t c = 0; c < channels_count; c++) {
            int ch = channels_to_apply[c];
            pipeline_exec_step_t* exec = &pipeline->steps[exec_idx++];
            exec->type = EXEC_STEP_FILTER;
            exec->bypassed = is_bypassed;
            exec->channel = ch;
            exec->filters_count = step->names_count;
            exec->filters =
                (filter_t**)calloc(step->names_count, sizeof(filter_t*));
            if (!exec->filters) {
              if (all_chs) free(all_chs);
              config_error_set(err, CONFIG_ERR_PARSE,
                               "Memory allocation failure");
              pipeline_free(pipeline);
              return NULL;
            }

            for (size_t j = 0; j < step->names_count; j++) {
              const filter_config_t* f_cfg =
                  dsp_config_get_filter(config, step->names[j]);
              if (!f_cfg) {
                if (all_chs) free(all_chs);
                config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                                 "Filter '%s' not defined", step->names[j]);
                pipeline_free(pipeline);
                return NULL;
              }
              filter_t* f =
                  filter_create(step->names[j], f_cfg, pipeline->rate,
                                pipeline->frames_per_chunk, proc_params);
              if (!f) {
                if (all_chs) free(all_chs);
                config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                                 "Failed to create filter '%s'",
                                 step->names[j]);
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
            config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                             "Mixer step missing name or config");
            pipeline_free(pipeline);
            return NULL;
          }
          const mixer_config_t* m_cfg =
              dsp_config_get_mixer(config, step->name);
          if (!m_cfg) {
            config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                             "Mixer step missing name or config");
            pipeline_free(pipeline);
            return NULL;
          }
          audio_mixer_t* m =
              audio_mixer_create(step->name, m_cfg, pipeline->frames_per_chunk);
          if (!m) {
            config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                             "Failed to create mixer '%s'", step->name);
            pipeline_free(pipeline);
            return NULL;
          }
          current_channels = m_cfg->channels_out;
          audio_chunk_t* scratch =
              audio_chunk_create(pipeline->frames_per_chunk, current_channels);
          if (!scratch) {
            audio_mixer_free(m);
            config_error_set(err, CONFIG_ERR_PARSE,
                             "Failed to allocate mixer scratch buffer");
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
            config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                             "Processor step missing name or config");
            pipeline_free(pipeline);
            return NULL;
          }
          const processor_config_t* p_cfg =
              dsp_config_get_processor(config, step->name);
          if (!p_cfg) {
            config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                             "Processor step missing name or config");
            pipeline_free(pipeline);
            return NULL;
          }
          dsp_processor_t* p = dsp_processor_create(
              step->name, p_cfg, pipeline->rate, pipeline->frames_per_chunk);
          if (!p) {
            config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                             "Failed to create processor '%s'", step->name);
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
pipeline_error_t pipeline_process(pipeline_t* pipeline,
                                  const audio_chunk_t* input,
                                  audio_chunk_t* output) {
  if (!pipeline || !input || !output) return PIPELINE_ERR_INPUT_SIZE_MISMATCH;
  size_t valid_frames = audio_chunk_get_valid_frames(input);

  // 1. Validate input and output buffer shapes/capacities against pipeline
  // configurations.
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
    mutable_waveform_t dst =
        audio_chunk_get_channel(pipeline->capture_scratch, ch);
    if (src && dst && valid_frames > 0) {
      memcpy(dst, src, valid_frames * sizeof(double));
    }
  }
  audio_chunk_set_valid_frames(pipeline->capture_scratch, valid_frames);

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
        if ((size_t)step->channel >= audio_chunk_get_channels(current_chunk))
          continue;
        mutable_waveform_t buf =
            audio_chunk_get_channel(current_chunk, step->channel);
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
        // Mixers process input from current_chunk and write to a pre-allocated
        // scratch buffer. This scratch buffer becomes the new current_chunk
        // for subsequent steps, handling potential channel count changes.
        mixer_error_t err =
            audio_mixer_process(step->mixer, current_chunk, scratch);
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
          pipeline->last_error_needed =
              audio_mixer_get_channels_in(step->mixer);
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

  // 5. Copy the final computed samples from workingChunk to caller-supplied
  // output buffer.
  audio_chunk_set_valid_frames(output, valid_frames);
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
void pipeline_update_parameters(pipeline_t* pipeline,
                                const dsp_config_t* config,
                                const char* const* filters,
                                size_t filters_count, const char* const* mixers,
                                size_t mixers_count,
                                const char* const* processors,
                                size_t processors_count) {
  if (!pipeline || !config) return;
  for (size_t i = 0; i < pipeline->steps_count; i++) {
    pipeline_exec_step_t* step = &pipeline->steps[i];
    switch (step->type) {
      case EXEC_STEP_FILTER:
        for (size_t j = 0; j < step->filters_count; j++) {
          filter_t* f = step->filters[j];
          if (f) {
            const char* f_name = filter_get_name(f);
            if (string_list_contains(filters, filters_count, f_name)) {
              filter_config_t* f_cfg = dsp_config_get_filter(config, f_name);
              if (f_cfg) {
                filter_update_parameters(f, f_cfg, pipeline->rate);
              }
            }
          }
        }
        break;
      case EXEC_STEP_MIXER: {
        const char* m_name = audio_mixer_get_name(step->mixer);
        if (step->mixer && m_name &&
            string_list_contains(mixers, mixers_count, m_name)) {
          mixer_config_t* m_cfg = dsp_config_get_mixer(config, m_name);
          if (m_cfg) {
            audio_mixer_update_parameters(step->mixer, m_cfg);
          }
        }
        break;
      }
      case EXEC_STEP_PROCESSOR: {
        const char* p_name = dsp_processor_get_name(step->processor);
        if (step->processor && p_name &&
            string_list_contains(processors, processors_count, p_name)) {
          processor_config_t* p_cfg = dsp_config_get_processor(config, p_name);
          if (p_cfg) {
            dsp_processor_update_parameters(step->processor, p_cfg,
                                            pipeline->rate);
          }
        }
        break;
      }
    }
  }
}

size_t pipeline_get_last_error_needed(const pipeline_t* pipeline) {
  return pipeline ? pipeline->last_error_needed : 0;
}

size_t pipeline_get_last_error_got(const pipeline_t* pipeline) {
  return pipeline ? pipeline->last_error_got : 0;
}
