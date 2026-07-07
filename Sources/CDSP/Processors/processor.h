#ifndef CLIB_PROCESSORS_PROCESSOR_H
#define CLIB_PROCESSORS_PROCESSOR_H

/**
 * @file processor.h
 * @brief Multi-channel audio processor interface and wrapper definitions.
 *
 * This module defines a polymorphic C interface (`dsp_processor_t`) that
 * mirrors the Swift `Processor` protocol for all multi-channel audio processors
 * (Compressor, Noise Gate, RACE). It provides uniform dispatch for real-time
 * in-place processing and dynamic parameter updates.
 */

#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Config/processor_config_types.h"
#include "compressor_processor.h"
#include "noise_gate_processor.h"
#include "race_processor.h"

/**
 * @brief Enumeration of concrete processor implementation types.
 */
typedef enum {
  PROCESSOR_IMPL_COMPRESSOR = 0,  ///< Dynamic range compressor processor.
  PROCESSOR_IMPL_NOISE_GATE,      ///< Noise gate processor.
  PROCESSOR_IMPL_RACE             ///< RACE cross-talk cancellation processor.
} processor_impl_type_t;

/// Protocol for all multi-channel audio processors.
typedef struct dsp_processor {
  processor_impl_type_t type;  ///< Concrete implementation type identifier.
  void* impl;                  ///< Pointer to underlying processor structure.

  /// Apply the processor to all channels of `chunk` in place.
  void (*process)(struct dsp_processor* self, audio_chunk_t* chunk);

  /// Update the processor parameters dynamically.
  void (*update_parameters)(struct dsp_processor* self,
                            const processor_config_t* config, int sample_rate);

  /// The unique name of this processor instance.
  const char* (*get_name)(const struct dsp_processor* self);

  /// Destructor function to free the processor and its wrapper.
  void (*free)(struct dsp_processor* self);
} dsp_processor_t;

/**
 * @brief Factory function to create a processor instance based on
 * configuration.
 *
 * @param name Unique name for this processor instance.
 * @param config Configuration specifying processor type and parameters.
 * @param sample_rate Audio sample rate in Hz.
 * @param chunk_size Maximum number of frames per processing chunk.
 * @return Pointer to newly allocated dsp_processor_t wrapper, or NULL on
 * failure.
 */
dsp_processor_t* dsp_processor_create(const char* name,
                                      const processor_config_t* config,
                                      int sample_rate, size_t chunk_size);

/**
 * @brief Wraps an existing compressor processor into the generic
 * dsp_processor_t interface.
 *
 * @param p Pointer to compressor processor.
 * @return Pointer to generic processor wrapper, or NULL on failure.
 */
dsp_processor_t* dsp_processor_wrap_compressor(compressor_processor_t* p);

/**
 * @brief Wraps an existing noise gate processor into the generic
 * dsp_processor_t interface.
 *
 * @param p Pointer to noise gate processor.
 * @return Pointer to generic processor wrapper, or NULL on failure.
 */
dsp_processor_t* dsp_processor_wrap_noise_gate(noise_gate_processor_t* p);

/**
 * @brief Wraps an existing RACE processor into the generic dsp_processor_t
 * interface.
 *
 * @param p Pointer to RACE processor.
 * @return Pointer to generic processor wrapper, or NULL on failure.
 */
dsp_processor_t* dsp_processor_wrap_race(race_processor_t* p);

/// Apply the processor to all channels of `chunk` in place.
static inline void dsp_processor_process(dsp_processor_t* proc,
                                         audio_chunk_t* chunk) {
  if (proc && proc->process) {
    proc->process(proc, chunk);
  }
}

/// Update the processor parameters dynamically.
static inline void dsp_processor_update_parameters(
    dsp_processor_t* proc, const processor_config_t* config, int sample_rate) {
  if (proc && proc->update_parameters) {
    proc->update_parameters(proc, config, sample_rate);
  }
}

/// The unique name of this processor instance.
static inline const char* dsp_processor_get_name(const dsp_processor_t* proc) {
  return (proc && proc->get_name) ? proc->get_name(proc) : "";
}

/**
 * @brief Frees the processor wrapper and its underlying implementation.
 *
 * @param proc Pointer to generic processor wrapper.
 */
static inline void dsp_processor_free(dsp_processor_t* proc) {
  if (proc && proc->free) {
    proc->free(proc);
  }
}

#endif  // CLIB_PROCESSORS_PROCESSOR_H
