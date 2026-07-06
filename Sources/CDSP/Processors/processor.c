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

#include <stdlib.h>
#include <string.h>

static void comp_process(dsp_processor_t* self, audio_chunk_t* chunk) {
  compressor_processor_process((compressor_processor_t*)self->impl, chunk);
}
static void comp_update(dsp_processor_t* self, const processor_config_t* config,
                        int sample_rate) {
  compressor_processor_update_parameters((compressor_processor_t*)self->impl,
                                         config, sample_rate);
}
static const char* comp_get_name(const dsp_processor_t* self) {
  return self->impl ? ((compressor_processor_t*)self->impl)->name : "";
}
static void comp_free(dsp_processor_t* self) {
  if (self->impl)
    compressor_processor_free((compressor_processor_t*)self->impl);
  free(self);
}

dsp_processor_t* dsp_processor_wrap_compressor(compressor_processor_t* p) {
  if (!p) return NULL;
  dsp_processor_t* wrap = (dsp_processor_t*)calloc(1, sizeof(dsp_processor_t));
  if (!wrap) {
    compressor_processor_free(p);
    return NULL;
  }
  wrap->type = PROCESSOR_IMPL_COMPRESSOR;
  wrap->impl = p;
  wrap->process = comp_process;
  wrap->update_parameters = comp_update;
  wrap->get_name = comp_get_name;
  wrap->free = comp_free;
  return wrap;
}

static void gate_process(dsp_processor_t* self, audio_chunk_t* chunk) {
  noise_gate_processor_process((noise_gate_processor_t*)self->impl, chunk);
}
static void gate_update(dsp_processor_t* self, const processor_config_t* config,
                        int sample_rate) {
  noise_gate_processor_update_parameters((noise_gate_processor_t*)self->impl,
                                         config, sample_rate);
}
static const char* gate_get_name(const dsp_processor_t* self) {
  return self->impl ? ((noise_gate_processor_t*)self->impl)->name : "";
}
static void gate_free(dsp_processor_t* self) {
  if (self->impl)
    noise_gate_processor_free((noise_gate_processor_t*)self->impl);
  free(self);
}

dsp_processor_t* dsp_processor_wrap_noise_gate(noise_gate_processor_t* p) {
  if (!p) return NULL;
  dsp_processor_t* wrap = (dsp_processor_t*)calloc(1, sizeof(dsp_processor_t));
  if (!wrap) {
    noise_gate_processor_free(p);
    return NULL;
  }
  wrap->type = PROCESSOR_IMPL_NOISE_GATE;
  wrap->impl = p;
  wrap->process = gate_process;
  wrap->update_parameters = gate_update;
  wrap->get_name = gate_get_name;
  wrap->free = gate_free;
  return wrap;
}

static void race_proc(dsp_processor_t* self, audio_chunk_t* chunk) {
  race_processor_process((race_processor_t*)self->impl, chunk);
}
static void race_update(dsp_processor_t* self, const processor_config_t* config,
                        int sample_rate) {
  race_processor_update_parameters((race_processor_t*)self->impl, config,
                                   sample_rate);
}
static const char* race_get_name(const dsp_processor_t* self) {
  return self->impl ? ((race_processor_t*)self->impl)->name : "";
}
static void race_free_fn(dsp_processor_t* self) {
  if (self->impl) race_processor_free((race_processor_t*)self->impl);
  free(self);
}

dsp_processor_t* dsp_processor_wrap_race(race_processor_t* p) {
  if (!p) return NULL;
  dsp_processor_t* wrap = (dsp_processor_t*)calloc(1, sizeof(dsp_processor_t));
  if (!wrap) {
    race_processor_free(p);
    return NULL;
  }
  wrap->type = PROCESSOR_IMPL_RACE;
  wrap->impl = p;
  wrap->process = race_proc;
  wrap->update_parameters = race_update;
  wrap->get_name = race_get_name;
  wrap->free = race_free_fn;
  return wrap;
}

dsp_processor_t* dsp_processor_create(const char* name,
                                      const processor_config_t* config,
                                      int sample_rate, size_t chunk_size) {
  if (!config) return NULL;
  switch (config->type) {
    case PROCESSOR_TYPE_COMPRESSOR: {
      compressor_processor_t* p = compressor_processor_create(
          name, &config->parameters.compressor, sample_rate, chunk_size);
      return dsp_processor_wrap_compressor(p);
    }
    case PROCESSOR_TYPE_NOISE_GATE: {
      noise_gate_processor_t* p = noise_gate_processor_create(
          name, &config->parameters.noise_gate, sample_rate, chunk_size);
      return dsp_processor_wrap_noise_gate(p);
    }
    case PROCESSOR_TYPE_RACE: {
      race_processor_t* p =
          race_processor_create(name, &config->parameters.race, sample_rate);
      return dsp_processor_wrap_race(p);
    }
    default:
      return NULL;
  }
}
