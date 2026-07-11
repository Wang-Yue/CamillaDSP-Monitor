#include "configuration.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Top-level configuration validation and memory management.

filter_config_t* dsp_config_get_filter(const dsp_config_t* config,
                                       const char* name) {
  if (!config || !name) return NULL;
  for (size_t i = 0; i < config->filters_count; i++) {
    if (strcmp(config->filters[i].name, name) == 0) {
      return &config->filters[i].filter;
    }
  }
  return NULL;
}

mixer_config_t* dsp_config_get_mixer(const dsp_config_t* config,
                                     const char* name) {
  if (!config || !name) return NULL;
  for (size_t i = 0; i < config->mixers_count; i++) {
    if (strcmp(config->mixers[i].name, name) == 0) {
      return &config->mixers[i].mixer;
    }
  }
  return NULL;
}

processor_config_t* dsp_config_get_processor(const dsp_config_t* config,
                                             const char* name) {
  if (!config || !name) return NULL;
  for (size_t i = 0; i < config->processors_count; i++) {
    if (strcmp(config->processors[i].name, name) == 0) {
      return &config->processors[i].processor;
    }
  }
  return NULL;
}

int dsp_config_validate(const dsp_config_t* config, config_error_t* err) {
  if (!config) return 0;

  // Top level checks
  if (config->devices.samplerate == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "Sample rate must be positive");
    return -1;
  }
  if (config->devices.chunksize == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION, "Chunk size must be positive");
    return -1;
  }
  if (!config->devices.capture.is_wav &&
      capture_device_config_get_channels(&config->devices.capture) <= 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "Capture channels must be positive");
    return -1;
  }
  if (playback_device_config_get_channels(&config->devices.playback) <= 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "Playback channels must be positive");
    return -1;
  }

  if (config->devices.has_silence_timeout &&
      config->devices.silence_timeout < 0.0) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "silence_timeout cannot be negative");
    return -1;
  }
  if (config->devices.has_silence_threshold &&
      config->devices.silence_threshold > 0.0) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "silence_threshold must be less than or equal to 0");
    return -1;
  }
  if (config->devices.has_volume_limit) {
    if (config->devices.volume_limit > 50.0) {
      config_error_set(err, CONFIG_ERR_VALIDATION,
                       "Volume limit cannot be above +50 dB");
      return -1;
    }
    if (config->devices.volume_limit < -150.0) {
      config_error_set(err, CONFIG_ERR_VALIDATION,
                       "Volume limit cannot be less than -150 dB");
      return -1;
    }
  }
  if (config->devices.has_target_level) {
    /* Target level is verified against a maximum theoretical limit.
     * The limit is buffer-dependent and differs for ALSA due to the larger
     * buffer requirements of the backend. */
    int64_t qlimit_val =
        config->devices.has_queuelimit ? config->devices.queuelimit : 4;
    if (qlimit_val < 0 || qlimit_val > 1000) {
      config_error_set(err, CONFIG_ERR_VALIDATION,
                       "queuelimit must be between 0 and 1000");
      return -1;
    }
    if (config->devices.chunksize <= 0 || config->devices.chunksize > 1000000) {
      config_error_set(err, CONFIG_ERR_VALIDATION,
                       "chunksize must be between 1 and 1000000");
      return -1;
    }
    int64_t target_limit = (2 + qlimit_val) * (int64_t)config->devices.chunksize;
#if defined(ENABLE_ALSA)
    if (config->devices.playback.type == AUDIO_BACKEND_TYPE_ALSA) {
      target_limit = (4 + qlimit_val) * (int64_t)config->devices.chunksize;
    }
#endif
    if ((int64_t)config->devices.target_level > target_limit ||
        config->devices.target_level <= 0) {
      char msg[128];
      snprintf(msg, sizeof(msg), "target_level must be between 1 and %lld",
               (long long)target_limit);
      config_error_set(err, CONFIG_ERR_VALIDATION, msg);
      return -1;
    }
  }

  if (config->devices.has_worker_threads &&
      config->devices.worker_threads <= 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "worker_threads must be positive");
    return -1;
  }

  if (config->devices.has_adjust_period && config->devices.adjust_period < 0.1) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "adjust_period must be at least 0.1 seconds");
    return -1;
  }

  // Validate filters
  for (size_t i = 0; i < config->filters_count; i++) {
    config_error_t sub_err;
    config_error_init(&sub_err);
    if (filter_config_validate(&config->filters[i].filter,
                               config->devices.samplerate, &sub_err) != 0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Filter '%s': %s",
                       config->filters[i].name, sub_err.message);
      return -1;
    }
  }

  // Validate mixers
  for (size_t i = 0; i < config->mixers_count; i++) {
    config_error_t sub_err;
    config_error_init(&sub_err);
    if (mixer_config_validate(&config->mixers[i].mixer, &sub_err) != 0) {
      config_error_set(err, CONFIG_ERR_INVALID_MIXER, "Mixer '%s': %s",
                       config->mixers[i].name, sub_err.message);
      return -1;
    }
  }

  // Validate processors
  for (size_t i = 0; i < config->processors_count; i++) {
    config_error_t sub_err;
    config_error_init(&sub_err);
    if (processor_config_validate(&config->processors[i].processor, &sub_err) !=
        0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Processor '%s': %s",
                       config->processors[i].name, sub_err.message);
      return -1;
    }
  }

  /* Validate pipeline structure and perform a channel-tracking walk.
   * We trace the number of channels available at each step. The pipeline begins
   * with capture channels, is mutated by mixers (which change the channel
   * count), and must end matching the expected playback device channels. */
  int num_channels =
      capture_device_config_get_channels(&config->devices.capture);
  for (size_t i = 0; i < config->pipeline_count; i++) {
    const pipeline_step_t* step = &config->pipeline[i];
    if (step->bypassed) continue;

    switch (step->type) {
      case PIPELINE_STEP_TYPE_FILTER: {
        if (!step->names || step->names_count == 0) {
          config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                           "Filter step %zu must have 'names'", i);
          return -1;
        }
        if (!step->has_channel &&
            (!step->channels || step->channels_count == 0)) {
          config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                           "Filter step %zu must have 'channel' or 'channels'",
                           i);
          return -1;
        }
        for (size_t j = 0; j < step->names_count; j++) {
          if (!step->names[j] || step->names[j][0] == '\0') {
            config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                             "Filter step %zu has invalid/empty filter name at index %zu",
                             i, j);
            return -1;
          }
          if (!dsp_config_get_filter(config, step->names[j])) {
            config_error_set(
                err, CONFIG_ERR_INVALID_PIPELINE,
                "Filter '%s' referenced in pipeline but not defined",
                step->names[j]);
            return -1;
          }
        }
        if (step->has_channel) {
          if (step->channel >= num_channels) {
            config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                             "Filter step %zu references channel %d but "
                             "pipeline only has %d channel(s) at this point",
                             i, step->channel, num_channels);
            return -1;
          }
        }
        for (size_t j = 0; j < step->channels_count; j++) {
          if (step->channels[j] >= num_channels) {
            config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                             "Filter step %zu references channel %d but "
                             "pipeline only has %d channel(s) at this point",
                             i, step->channels[j], num_channels);
            return -1;
          }
          for (size_t k = 0; k < j; k++) {
            if (step->channels[j] == step->channels[k]) {
              config_error_set(
                  err, CONFIG_ERR_INVALID_PIPELINE,
                  "Filter step %zu references duplicated channel %d", i,
                  step->channels[j]);
              return -1;
            }
          }
        }
        break;
      }
      case PIPELINE_STEP_TYPE_MIXER: {
        /* Mixers change the channel layout. Verify that the mixer's input
         * channels match the current pipeline state, and then update the
         * tracked channel count. */
        if (!step->has_name || !step->name || step->name[0] == '\0') {
          config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                           "Mixer step %zu must have 'name'", i);
          return -1;
        }
        const mixer_config_t* mixer = dsp_config_get_mixer(config, step->name);
        if (!mixer) {
          config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                           "Mixer '%s' referenced in pipeline but not defined",
                           step->name);
          return -1;
        }
        if (mixer->channels_in != (size_t)num_channels) {
          config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                           "Mixer '%s' expects %d input channel(s) but "
                           "pipeline has %d at this point",
                           step->name, mixer->channels_in, num_channels);
          return -1;
        }
        num_channels = mixer->channels_out;
        break;
      }
      case PIPELINE_STEP_TYPE_PROCESSOR: {
        /* Processors must match the channel count of the pipeline at the
         * insertion point. */
        if (!step->has_name || !step->name || step->name[0] == '\0') {
          config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                           "Processor step %zu must have 'name'", i);
          return -1;
        }
        const processor_config_t* proc =
            dsp_config_get_processor(config, step->name);
        if (!proc) {
          config_error_set(
              err, CONFIG_ERR_INVALID_PIPELINE,
              "Processor '%s' referenced in pipeline but not defined",
              step->name);
          return -1;
        }
        int expected_channels = 0;
        switch (proc->type) {
          case PROCESSOR_TYPE_COMPRESSOR:
            expected_channels = proc->parameters.compressor.channels;
            break;
          case PROCESSOR_TYPE_NOISE_GATE:
            expected_channels = proc->parameters.noise_gate.channels;
            break;
          case PROCESSOR_TYPE_RACE:
            expected_channels = proc->parameters.race.channels;
            break;
        }
        if (expected_channels != num_channels) {
          config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                           "Processor '%s' expects %d channel(s) but pipeline "
                           "has %d at this point",
                           step->name, expected_channels, num_channels);
          return -1;
        }
        break;
      }
    }
  }

  /* Final verification: The output channel count of the pipeline must match the
   * playback configuration. */
  int playback_channels =
      playback_device_config_get_channels(&config->devices.playback);
  if (num_channels != playback_channels) {
    config_error_set(
        err, CONFIG_ERR_INVALID_PIPELINE,
        "Pipeline outputs %d channel(s) but playback device expects %d",
        num_channels, playback_channels);
    return -1;
  }

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
        free(config->processors[i]
                 .processor.parameters.compressor.monitor_channels);
        free(config->processors[i]
                 .processor.parameters.compressor.process_channels);
      } else if (config->processors[i].processor.type ==
                 PROCESSOR_TYPE_NOISE_GATE) {
        free(config->processors[i]
                 .processor.parameters.noise_gate.monitor_channels);
        free(config->processors[i]
                 .processor.parameters.noise_gate.process_channels);
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
  if (config->devices.capture.has_labels && config->devices.capture.labels) {
    for (size_t i = 0; i < config->devices.capture.labels_count; i++) {
      free(config->devices.capture.labels[i]);
    }
    free(config->devices.capture.labels);
  }
  if (config->devices.playback.has_labels && config->devices.playback.labels) {
    for (size_t i = 0; i < config->devices.playback.labels_count; i++) {
      free(config->devices.playback.labels[i]);
    }
    free(config->devices.playback.labels);
  }
  free(config);
}
