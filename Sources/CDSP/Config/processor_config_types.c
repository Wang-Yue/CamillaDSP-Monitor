#include "processor_config_types.h"

#include <stdio.h>
#include <string.h>

const char* processor_type_to_string(processor_type_t type) {
  switch (type) {
    case PROCESSOR_TYPE_COMPRESSOR:
      return "Compressor";
    case PROCESSOR_TYPE_NOISE_GATE:
      return "NoiseGate";
    case PROCESSOR_TYPE_RACE:
      return "RACE";
    default:
      return "Compressor";
  }
}

processor_type_t processor_type_from_string(const char* str) {
  if (!str) return PROCESSOR_TYPE_COMPRESSOR;
  if (strcmp(str, "Compressor") == 0) return PROCESSOR_TYPE_COMPRESSOR;
  if (strcmp(str, "NoiseGate") == 0) return PROCESSOR_TYPE_NOISE_GATE;
  if (strcmp(str, "RACE") == 0) return PROCESSOR_TYPE_RACE;
  return PROCESSOR_TYPE_COMPRESSOR;
}

int processor_config_validate(const processor_config_t* proc,
                              config_error_t* err) {
  if (!proc) return 0;
  switch (proc->type) {
    case PROCESSOR_TYPE_COMPRESSOR: {
      const compressor_parameters_t* p = &proc->parameters.compressor;
      if (p->channels <= 0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Compressor: channels must be > 0, got %d", p->channels);
        return -1;
      }
      if (p->attack <= 0.0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Compressor: attack must be > 0, got %g", p->attack);
        return -1;
      }
      if (p->release <= 0.0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Compressor: release must be > 0, got %g", p->release);
        return -1;
      }
      for (size_t i = 0; i < p->monitor_channels_count; i++) {
        if (p->monitor_channels[i] < 0 || p->monitor_channels[i] >= p->channels) {
          config_error_set(
              err, CONFIG_ERR_INVALID_FILTER,
              "Compressor: monitor channel %d is invalid (max: %d)",
              p->monitor_channels[i], p->channels - 1);
          return -1;
        }
      }
      for (size_t i = 0; i < p->process_channels_count; i++) {
        if (p->process_channels[i] < 0 || p->process_channels[i] >= p->channels) {
          config_error_set(
              err, CONFIG_ERR_INVALID_FILTER,
              "Compressor: process channel %d is invalid (max: %d)",
              p->process_channels[i], p->channels - 1);
          return -1;
        }
      }
      break;
    }
    case PROCESSOR_TYPE_NOISE_GATE: {
      const noise_gate_parameters_t* p = &proc->parameters.noise_gate;
      if (p->channels <= 0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "NoiseGate: channels must be > 0, got %d", p->channels);
        return -1;
      }
      if (p->attack <= 0.0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "NoiseGate: attack must be > 0, got %g", p->attack);
        return -1;
      }
      if (p->release <= 0.0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "NoiseGate: release must be > 0, got %g", p->release);
        return -1;
      }
      for (size_t i = 0; i < p->monitor_channels_count; i++) {
        if (p->monitor_channels[i] < 0 || p->monitor_channels[i] >= p->channels) {
          config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                           "NoiseGate: monitor channel %d is invalid (max: %d)",
                           p->monitor_channels[i], p->channels - 1);
          return -1;
        }
      }
      for (size_t i = 0; i < p->process_channels_count; i++) {
        if (p->process_channels[i] < 0 || p->process_channels[i] >= p->channels) {
          config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                           "NoiseGate: process channel %d is invalid (max: %d)",
                           p->process_channels[i], p->channels - 1);
          return -1;
        }
      }
      break;
    }
    case PROCESSOR_TYPE_RACE: {
      const race_parameters_t* p = &proc->parameters.race;
      if (p->channels <= 0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "RACE: channels must be > 0, got %d", p->channels);
        return -1;
      }
      if (p->attenuation <= 0.0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "RACE: attenuation must be > 0, got %g",
                         p->attenuation);
        return -1;
      }
      if (p->delay <= 0.0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "RACE: delay must be > 0, got %g", p->delay);
        return -1;
      }
      if (p->channel_a == p->channel_b) {
        config_error_set(
            err, CONFIG_ERR_INVALID_FILTER,
            "RACE: channels A and B must be different, got both %d",
            p->channel_a);
        return -1;
      }
      if (p->channel_a < 0 || p->channel_a >= p->channels) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "RACE: channel A %d is invalid (max: %d)",
                         p->channel_a, p->channels - 1);
        return -1;
      }
      if (p->channel_b < 0 || p->channel_b >= p->channels) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "RACE: channel B %d is invalid (max: %d)",
                         p->channel_b, p->channels - 1);
        return -1;
      }
      break;
    }
  }
  return 0;
}
