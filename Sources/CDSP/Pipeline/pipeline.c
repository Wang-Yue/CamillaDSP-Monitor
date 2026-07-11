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

#if defined(__APPLE__) || defined(USE_LIBDISPATCH)
#include <dispatch/dispatch.h>
#define HAS_DISPATCH 1
#else
#define HAS_DISPATCH 0
#endif

#include "Audio/audio_buffers.h"
#include "Audio/double_helpers.h"
#include "Filters/filter.h"
#include "Filters/volume.h"
#include "Mixer/mixer.h"
#include "Processors/processor.h"

/// A filter chain applied to a single channel in parallel.
typedef struct {
  int channel;
  filter_t** filters;
  size_t filters_count;
} parallel_filter_chain_t;

/// A single step in the processing pipeline
typedef enum {
  /// Contiguous filter chains that can be processed in parallel.
  EXEC_STEP_PARALLEL_FILTERS = 0,
  /// Mixer that changes channel routing.
  EXEC_STEP_MIXER,
  /// Audio processor applied to the chunk in-place.
  EXEC_STEP_PROCESSOR
} exec_step_type_t;

/// A single step in the processing pipeline
typedef struct {
  exec_step_type_t type;
  // For EXEC_STEP_PARALLEL_FILTERS:
  parallel_filter_chain_t* chains;
  size_t chains_count;
  // For EXEC_STEP_MIXER:
  audio_mixer_t* mixer;
  // For EXEC_STEP_PROCESSOR:
  dsp_processor_t* processor;
} pipeline_exec_step_t;

/// The main audio processing pipeline.
struct pipeline_s {
  pipeline_exec_step_t* steps;
  size_t steps_count;
  bool multithreaded;
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

static void free_filter_chains(parallel_filter_chain_t* chains, size_t count) {
  if (!chains) return;
  for (size_t i = 0; i < count; i++) {
    if (chains[i].filters) {
      for (size_t j = 0; j < chains[i].filters_count; j++) {
        if (chains[i].filters[j]) {
          filter_free(chains[i].filters[j]);
        }
      }
      free(chains[i].filters);
    }
  }
  free(chains);
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

    if (dest_step->type == EXEC_STEP_PARALLEL_FILTERS) {
      for (size_t dc = 0; dc < dest_step->chains_count; dc++) {
        parallel_filter_chain_t* dest_chain = &dest_step->chains[dc];
        // Find matching filter chain in src steps by channel
        for (size_t s = 0; s < src->steps_count; s++) {
          const pipeline_exec_step_t* src_step = &src->steps[s];
          if (src_step->type == EXEC_STEP_PARALLEL_FILTERS) {
            for (size_t sc = 0; sc < src_step->chains_count; sc++) {
              const parallel_filter_chain_t* src_chain = &src_step->chains[sc];
              if (src_chain->channel == dest_chain->channel) {
                // Transfer individual filters by name matching
                for (size_t df = 0; df < dest_chain->filters_count; df++) {
                  filter_t* dest_f = dest_chain->filters[df];
                  const char* dest_name = filter_get_name(dest_f);
                  for (size_t sf = 0; sf < src_chain->filters_count; sf++) {
                    filter_t* src_f = src_chain->filters[sf];
                    if (strcmp(filter_get_name(src_f), dest_name) == 0) {
                      filter_transfer_state(dest_f, src_f);
                      break;
                    }
                  }
                }
                break;
              }
            }
          }
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
            dsp_processor_transfer_state(dest_step->processor,
                                         src_step->processor);
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
      if (step->type == EXEC_STEP_PARALLEL_FILTERS) {
        free_filter_chains(step->chains, step->chains_count);
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
  pipeline->multithreaded =
      config->devices.has_multithreaded ? config->devices.multithreaded : false;

  // Create the implicit master volume filter
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
                           pipeline->frames_per_chunk, proc_params, err);
  if (!pipeline->master_volume) {
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
  // needed.
  if (config->pipeline && config->pipeline_count > 0) {
    for (size_t i = 0; i < config->pipeline_count; i++) {
      const pipeline_step_t* step = &config->pipeline[i];
      if (step->bypassed) continue;
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

  if (total_exec_steps == 0) {
    pipeline->steps_count = 0;
    pipeline->expected_out_channels = current_channels;
    return pipeline;
  }

  pipeline->steps = (pipeline_exec_step_t*)calloc(
      total_exec_steps, sizeof(pipeline_exec_step_t));
  if (!pipeline->steps) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    pipeline_free(pipeline);
    return NULL;
  }
  pipeline->steps_count = total_exec_steps;
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
      if (step->bypassed) continue;
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
            if (current_channels > SIZE_MAX / sizeof(int)) {
              config_error_set(err, CONFIG_ERR_PARSE,
                               "Integer overflow in channels count");
              pipeline_free(pipeline);
              return NULL;
            }
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

          // Create chains for this filter step
          size_t new_chains_count = channels_count;
          parallel_filter_chain_t* new_chains =
              (parallel_filter_chain_t*)calloc(new_chains_count,
                                               sizeof(parallel_filter_chain_t));
          if (!new_chains) {
            if (all_chs) free(all_chs);
            config_error_set(err, CONFIG_ERR_PARSE,
                             "Memory allocation failure");
            pipeline_free(pipeline);
            return NULL;
          }

          for (size_t c = 0; c < channels_count; c++) {
            int ch = channels_to_apply[c];
            parallel_filter_chain_t* chain = &new_chains[c];
            chain->channel = ch;
            chain->filters_count = step->names_count;
            chain->filters =
                (filter_t**)calloc(step->names_count, sizeof(filter_t*));
            if (!chain->filters) {
              if (all_chs) free(all_chs);
              free_filter_chains(new_chains, channels_count);
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
                free_filter_chains(new_chains, channels_count);
                config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                                 "Filter '%s' not defined", step->names[j]);
                pipeline_free(pipeline);
                return NULL;
              }
              filter_t* f =
                  filter_create(step->names[j], f_cfg, pipeline->rate,
                                pipeline->frames_per_chunk, proc_params, err);
              if (!f) {
                if (all_chs) free(all_chs);
                free_filter_chains(new_chains, channels_count);
                pipeline_free(pipeline);
                return NULL;
              }
              chain->filters[j] = f;
            }
          }
          if (all_chs) free(all_chs);

          // Merge adjacent parallel filters, combining filter lists for the
          // same channels
          if (exec_idx > 0 && pipeline->steps[exec_idx - 1].type ==
                                  EXEC_STEP_PARALLEL_FILTERS) {
            pipeline_exec_step_t* last = &pipeline->steps[exec_idx - 1];
            bool alloc_failed = false;

            for (size_t c = 0; c < new_chains_count; c++) {
              parallel_filter_chain_t* new_chain = &new_chains[c];
              int found_idx = -1;
              for (size_t k = 0; k < last->chains_count; k++) {
                if (last->chains[k].channel == new_chain->channel) {
                  found_idx = (int)k;
                  break;
                }
              }

              if (found_idx != -1) {
                // Channel exists. Combine filters.
                parallel_filter_chain_t* old_chain = &last->chains[found_idx];
                size_t combined_count =
                    old_chain->filters_count + new_chain->filters_count;
                filter_t** combined_filters = realloc(
                    old_chain->filters, combined_count * sizeof(filter_t*));
                if (!combined_filters) {
                  alloc_failed = true;
                  break;
                }
                memcpy(combined_filters + old_chain->filters_count,
                       new_chain->filters,
                       new_chain->filters_count * sizeof(filter_t*));
                old_chain->filters = combined_filters;
                old_chain->filters_count = combined_count;
                free(new_chain->filters);
                new_chain->filters = NULL;
              } else {
                // Channel does not exist. Append the new chain.
                parallel_filter_chain_t* merged =
                    realloc(last->chains, (last->chains_count + 1) *
                                              sizeof(parallel_filter_chain_t));
                if (!merged) {
                  alloc_failed = true;
                  break;
                }
                merged[last->chains_count] = *new_chain;
                last->chains = merged;
                last->chains_count += 1;
                new_chain->filters = NULL;
              }
            }

            if (alloc_failed) {
              free_filter_chains(new_chains, new_chains_count);
              config_error_set(err, CONFIG_ERR_PARSE,
                               "Memory allocation failure");
              pipeline_free(pipeline);
              return NULL;
            }
            free(new_chains);
          } else {
            for (size_t c = 0; c < new_chains_count; c++) {
              for (size_t k = 0; k < c; k++) {
                if (new_chains[c].channel == new_chains[k].channel) {
                  free_filter_chains(new_chains, new_chains_count);
                  config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                                   "Duplicate channel %d in parallel filter step",
                                   new_chains[c].channel);
                  pipeline_free(pipeline);
                  return NULL;
                }
              }
            }
            pipeline_exec_step_t* exec = &pipeline->steps[exec_idx++];
            exec->type = EXEC_STEP_PARALLEL_FILTERS;
            exec->chains = new_chains;
            exec->chains_count = new_chains_count;
          }
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
              step->name, p_cfg, pipeline->rate, pipeline->frames_per_chunk, err);
          if (!p) {
            pipeline_free(pipeline);
            return NULL;
          }
          pipeline_exec_step_t* exec = &pipeline->steps[exec_idx++];
          exec->type = EXEC_STEP_PROCESSOR;
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

#if HAS_DISPATCH
typedef struct {
  audio_chunk_t* current_chunk;
  size_t valid_frames;
  parallel_filter_chain_t* chains;
} dispatch_ctx_t;

static void parallel_filter_worker(void* context, size_t idx) {
  dispatch_ctx_t* ctx = (dispatch_ctx_t*)context;
  parallel_filter_chain_t* chain = &ctx->chains[idx];
  if ((size_t)chain->channel >= audio_chunk_get_channels(ctx->current_chunk))
    return;
  mutable_waveform_t buf =
      audio_chunk_get_channel(ctx->current_chunk, chain->channel);
  if (!buf) return;
  for (size_t j = 0; j < chain->filters_count; j++) {
    if (chain->filters[j] && ctx->valid_frames > 0) {
      filter_process(chain->filters[j], buf, ctx->valid_frames);
    }
  }
}
#endif

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

  // 2. Copy input into our pre-allocated scratch.
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
      case EXEC_STEP_PARALLEL_FILTERS: {
        bool use_multithreading = false;
#if HAS_DISPATCH || defined(USE_OPENMP)
        if (pipeline->multithreaded && step->chains_count > 1) {
          use_multithreading = true;
        }
#endif

        if (use_multithreading) {
#if HAS_DISPATCH
          dispatch_ctx_t dctx = {current_chunk, valid_frames, step->chains};
          dispatch_queue_t queue =
              dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0);
          dispatch_apply_f(step->chains_count, queue, &dctx,
                           parallel_filter_worker);
#elif defined(USE_OPENMP)
#pragma omp parallel for num_threads(step->chains_count)
          for (size_t idx = 0; idx < step->chains_count; idx++) {
            parallel_filter_chain_t* chain = &step->chains[idx];
            if ((size_t)chain->channel >=
                audio_chunk_get_channels(current_chunk))
              continue;
            mutable_waveform_t buf =
                audio_chunk_get_channel(current_chunk, chain->channel);
            if (!buf) continue;
            for (size_t j = 0; j < chain->filters_count; j++) {
              if (chain->filters[j] && valid_frames > 0) {
                filter_process(chain->filters[j], buf, valid_frames);
              }
            }
          }
#endif
        } else {
          for (size_t idx = 0; idx < step->chains_count; idx++) {
            parallel_filter_chain_t* chain = &step->chains[idx];
            if ((size_t)chain->channel >=
                audio_chunk_get_channels(current_chunk))
              continue;
            mutable_waveform_t buf =
                audio_chunk_get_channel(current_chunk, chain->channel);
            if (!buf) continue;
            for (size_t j = 0; j < chain->filters_count; j++) {
              if (chain->filters[j] && valid_frames > 0) {
                filter_process(chain->filters[j], buf, valid_frames);
              }
            }
          }
        }
        break;
      }
      case EXEC_STEP_MIXER: {
        if (mixer_idx >= pipeline->scratches_for_mixers_count) continue;
        audio_chunk_t* scratch = pipeline->scratches_for_mixers[mixer_idx];
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

size_t pipeline_get_last_error_needed(const pipeline_t* pipeline) {
  return pipeline ? pipeline->last_error_needed : 0;
}

size_t pipeline_get_last_error_got(const pipeline_t* pipeline) {
  return pipeline ? pipeline->last_error_got : 0;
}
