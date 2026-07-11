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

typedef struct dsp_processor dsp_processor_t;

/**
 * @brief Factory function to create a processor instance based on
 * configuration.
 *
 * @param name Unique name for this processor instance.
 * @param config Configuration specifying processor type and parameters.
 * @param sample_rate Audio sample rate in Hz.
 * @param chunk_size Maximum number of frames per processing chunk.
 * @param err Pointer to a config error struct to populate on failure.
 * @return Pointer to newly allocated dsp_processor_t wrapper, or NULL on
 * failure.
 */
dsp_processor_t* dsp_processor_create(const char* name,
                                      const processor_config_t* config,
                                      int sample_rate, size_t chunk_size,
                                      config_error_t* err);

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

/**
 * @brief Applies the processor to all channels of the audio chunk in place.
 *
 * @param proc Pointer to the generic processor wrapper.
 * @param chunk Audio chunk to process in place.
 */
void dsp_processor_process(dsp_processor_t* proc, audio_chunk_t* chunk);

/**
 * @brief Transfers internal history state (loudness envelope, feedback, etc.)
 * from src to dest.
 *
 * @param dest The destination processor wrapper instance.
 * @param src The source processor wrapper instance.
 */
void dsp_processor_transfer_state(dsp_processor_t* dest,
                                  const dsp_processor_t* src);

/**
 * @brief Gets the unique name of this processor instance.
 *
 * @param proc Pointer to the generic processor wrapper.
 * @return The unique name of the processor instance.
 */
const char* dsp_processor_get_name(const dsp_processor_t* proc);

/**
 * @brief Frees the processor wrapper and its underlying implementation.
 *
 * @param proc Pointer to generic processor wrapper.
 */
void dsp_processor_free(dsp_processor_t* proc);

#endif  // CLIB_PROCESSORS_PROCESSOR_H
