/**
 * @file processor.c
 * @brief Implementation of polymorphic processor wrapper and factory dispatch.
 *
 * This implementation provides concrete dispatch tables for each supported
 * audio processor type, routing real-time processing and parameter updates to
 * the appropriate underlying processor (compressor, noise gate, or RACE
 * cross-talk cancellation).
 */

#include "processor.h"

struct dsp_processor {
  processor_impl_type_t type;  ///< Concrete implementation type identifier.
  void* impl;                  ///< Pointer to underlying processor structure.

  /// Apply the processor to all channels of `chunk` in place.
  void (*process)(struct dsp_processor* self, audio_chunk_t* chunk);

  /// The unique name of this processor instance.
  const char* (*get_name)(const struct dsp_processor* self);

  /// Destructor function to free the processor and its wrapper.
  void (*free)(struct dsp_processor* self);
};

void dsp_processor_process(dsp_processor_t* proc, audio_chunk_t* chunk) {
  if (proc && proc->process) {
    proc->process(proc, chunk);
  }
}

void dsp_processor_transfer_state(dsp_processor_t* dest,
                                  const dsp_processor_t* src) {
  if (!dest || !src) return;
  if (dest->type != src->type) return;
  switch (dest->type) {
    case PROCESSOR_IMPL_COMPRESSOR:
      compressor_processor_transfer_state(
          (compressor_processor_t*)dest->impl,
          (const compressor_processor_t*)src->impl);
      break;
    case PROCESSOR_IMPL_NOISE_GATE:
      noise_gate_processor_transfer_state(
          (noise_gate_processor_t*)dest->impl,
          (const noise_gate_processor_t*)src->impl);
      break;
    case PROCESSOR_IMPL_RACE:
      race_processor_transfer_state((race_processor_t*)dest->impl,
                                    (const race_processor_t*)src->impl);
      break;
  }
}

const char* dsp_processor_get_name(const dsp_processor_t* proc) {
  return (proc && proc->get_name) ? proc->get_name(proc) : "";
}

void dsp_processor_free(dsp_processor_t* proc) {
  if (proc && proc->free) {
    proc->free(proc);
  }
}

#include <stdlib.h>
#include <string.h>

/**
 * @brief Processes an audio chunk using the wrapped compressor.
 * @param self The wrapper processor instance.
 * @param chunk The audio chunk to be processed in-place.
 */
static void comp_process(dsp_processor_t* self, audio_chunk_t* chunk) {
  compressor_processor_process((compressor_processor_t*)self->impl, chunk);
}

/**
 * @brief Updates compressor parameters.
 * @param self The wrapper processor instance.
 * @param config The new configuration parameters.
 * @param sample_rate The current audio sample rate in Hz.
 */

/**
 * @brief Retrieves the name of the wrapped compressor.
 * @param self The wrapper processor instance.
 * @return The name string, or empty string if implementation is NULL.
 */
static const char* comp_get_name(const dsp_processor_t* self) {
  return self->impl ? compressor_processor_get_name(
                          (compressor_processor_t*)self->impl)
                    : "";
}

/**
 * @brief Frees the compressor implementation and the wrapper instance.
 * @param self The wrapper processor instance to free.
 */
static void comp_free(dsp_processor_t* self) {
  if (self->impl)
    compressor_processor_free((compressor_processor_t*)self->impl);
  free(self);
}

dsp_processor_t* dsp_processor_wrap_compressor(compressor_processor_t* p) {
  if (!p) return NULL;
  dsp_processor_t* wrap = (dsp_processor_t*)calloc(1, sizeof(dsp_processor_t));
  if (!wrap) {
    /* If wrapper allocation fails, ensure the passed processor instance is
       freed to prevent resource leaks. */
    compressor_processor_free(p);
    return NULL;
  }
  wrap->type = PROCESSOR_IMPL_COMPRESSOR;
  wrap->impl = p;
  wrap->process = comp_process;
  wrap->get_name = comp_get_name;
  wrap->free = comp_free;
  return wrap;
}

/**
 * @brief Processes an audio chunk using the wrapped noise gate.
 * @param self The wrapper processor instance.
 * @param chunk The audio chunk to be processed in-place.
 */
static void gate_process(dsp_processor_t* self, audio_chunk_t* chunk) {
  noise_gate_processor_process((noise_gate_processor_t*)self->impl, chunk);
}

/**
 * @brief Retrieves the name of the wrapped noise gate.
 * @param self The wrapper processor instance.
 * @return The name string, or empty string if implementation is NULL.
 */
static const char* gate_get_name(const dsp_processor_t* self) {
  return self->impl ? noise_gate_processor_get_name(
                          (noise_gate_processor_t*)self->impl)
                    : "";
}

/**
 * @brief Frees the noise gate implementation and the wrapper instance.
 * @param self The wrapper processor instance to free.
 */
static void gate_free(dsp_processor_t* self) {
  if (self->impl)
    noise_gate_processor_free((noise_gate_processor_t*)self->impl);
  free(self);
}

dsp_processor_t* dsp_processor_wrap_noise_gate(noise_gate_processor_t* p) {
  if (!p) return NULL;
  dsp_processor_t* wrap = (dsp_processor_t*)calloc(1, sizeof(dsp_processor_t));
  if (!wrap) {
    /* If wrapper allocation fails, ensure the passed processor instance is
       freed to prevent resource leaks. */
    noise_gate_processor_free(p);
    return NULL;
  }
  wrap->type = PROCESSOR_IMPL_NOISE_GATE;
  wrap->impl = p;
  wrap->process = gate_process;
  wrap->get_name = gate_get_name;
  wrap->free = gate_free;
  return wrap;
}

/**
 * @brief Processes an audio chunk using the wrapped RACE processor.
 * @param self The wrapper processor instance.
 * @param chunk The audio chunk to be processed in-place.
 */
static void race_proc(dsp_processor_t* self, audio_chunk_t* chunk) {
  race_processor_process((race_processor_t*)self->impl, chunk);
}

/**
 * @brief Retrieves the name of the wrapped RACE processor.
 * @param self The wrapper processor instance.
 * @return The name string, or empty string if implementation is NULL.
 */
static const char* race_get_name(const dsp_processor_t* self) {
  return self->impl ? race_processor_get_name((race_processor_t*)self->impl)
                    : "";
}

/**
 * @brief Frees the RACE processor implementation and the wrapper instance.
 * @param self The wrapper processor instance to free.
 */
static void race_free_fn(dsp_processor_t* self) {
  if (self->impl) race_processor_free((race_processor_t*)self->impl);
  free(self);
}

dsp_processor_t* dsp_processor_wrap_race(race_processor_t* p) {
  if (!p) return NULL;
  dsp_processor_t* wrap = (dsp_processor_t*)calloc(1, sizeof(dsp_processor_t));
  if (!wrap) {
    /* If wrapper allocation fails, ensure the passed processor instance is
       freed to prevent resource leaks. */
    race_processor_free(p);
    return NULL;
  }
  wrap->type = PROCESSOR_IMPL_RACE;
  wrap->impl = p;
  wrap->process = race_proc;
  wrap->get_name = race_get_name;
  wrap->free = race_free_fn;
  return wrap;
}

dsp_processor_t* dsp_processor_create(const char* name,
                                      const processor_config_t* config,
                                      int sample_rate, size_t chunk_size,
                                      config_error_t* err) {
  if (!config) {
    config_error_set(err, CONFIG_ERR_PARSE, "Processor config is NULL");
    return NULL;
  }
  switch (config->type) {
    case PROCESSOR_TYPE_COMPRESSOR: {
      compressor_processor_t* p = compressor_processor_create(
          name, &config->parameters.compressor, sample_rate, chunk_size);
      if (!p) {
        config_error_set(err, CONFIG_ERR_PARSE, "Failed to create compressor processor '%s'", name ? name : "");
        return NULL;
      }
      dsp_processor_t* wrap = dsp_processor_wrap_compressor(p);
      if (!wrap) {
        config_error_set(err, CONFIG_ERR_PARSE, "Failed to wrap compressor processor '%s'", name ? name : "");
      }
      return wrap;
    }
    case PROCESSOR_TYPE_NOISE_GATE: {
      noise_gate_processor_t* p = noise_gate_processor_create(
          name, &config->parameters.noise_gate, sample_rate, chunk_size);
      if (!p) {
        config_error_set(err, CONFIG_ERR_PARSE, "Failed to create noise gate processor '%s'", name ? name : "");
        return NULL;
      }
      dsp_processor_t* wrap = dsp_processor_wrap_noise_gate(p);
      if (!wrap) {
        config_error_set(err, CONFIG_ERR_PARSE, "Failed to wrap noise gate processor '%s'", name ? name : "");
      }
      return wrap;
    }
    case PROCESSOR_TYPE_RACE: {
      race_processor_t* p =
          race_processor_create(name, &config->parameters.race, sample_rate, err);
      if (!p) return NULL;
      dsp_processor_t* wrap = dsp_processor_wrap_race(p);
      if (!wrap) {
        config_error_set(err, CONFIG_ERR_PARSE, "Failed to wrap RACE processor '%s'", name ? name : "");
      }
      return wrap;
    }
    default:
      config_error_set(err, CONFIG_ERR_PARSE, "Unknown processor type %d for '%s'", config->type, name ? name : "");
      return NULL;
  }
}
