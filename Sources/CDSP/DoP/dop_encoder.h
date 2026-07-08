/**
 * @file dop_encoder.h
 * @brief PCM to DoP (DSD-over-PCM) encoder.
 *
 * Converts a chunk of PCM audio at the carrier rate into DSD-over-PCM, in
 * place. For each input frame it:
 * 1. Interpolates 16x to the DSD rate using a 511-tap beta=11 Kaiser-windowed
 *    polyphase sinc (same shape as the decoder, normalized per phase
 *    for unit DC gain).
 * 2. Modulates the oversampled signal with a per-channel sigma-delta
 *    modulator (using the configured `SDMFilter`, defaulting to `sdm-6`).
 * 3. Packs the 16 resulting DSD bits into the lower 16 bits of a 24-bit
 *    container, with an alternating `0x05` / `0xFA` marker in the upper byte.
 *
 * The encoded chunk satisfies the strict-alternation detection state
 * machine in `DoPDecoder` and round-trips through any DAC that natively
 * understands DoP. To preserve the bit pattern through CoreAudio the
 * playback format must be S24 or S32 (F32 will quantize the marker
 * away); the encoder itself just emits float-normalised 24-bit values
 * and trusts the playback backend to forward them losslessly.
 *
 * SDM state per channel is carried by an embedded `SigmaDeltaModulator`;
 * the polyphase coefficient table is shared across channels and built
 * once at init.
 */

#ifndef CLIB_DOP_DOP_ENCODER_H
#define CLIB_DOP_DOP_ENCODER_H

#include <stdbool.h>

#include "Audio/audio_chunk.h"
#include "Config/engine_config_types.h"

/**
 * @brief DoP encoder context.
 */
typedef struct dop_encoder dop_encoder_t;

/**
 * @brief Construct a DoP encoder.
 *
 * Always succeeds, but only actually encodes when `output_dop` is true and
 * `sample_rate` is a supported carrier rate. Otherwise, the encoder is
 * disabled and `dop_encoder_encode` becomes a no-op.
 *
 * @param channels Number of audio channels.
 * @param sample_rate The PCM sample rate (carrier rate).
 * @param output_dop If true, enables DoP encoding.
 * @param filter_name Noise-shaper filter name.
 * @param cutoff_hz Passband cutoff of the interpolation filter.
 *                  Ignored when `enabled` is false.
 * @return Pointer to the created dop_encoder_t instance.
 */
dop_encoder_t* dop_encoder_create(int channels, double sample_rate,
                                  bool output_dop, sdm_filter_t filter_name,
                                  double cutoff_hz);

/**
 * @brief Encode the chunk's PCM samples into DoP, in place.
 *
 * No-op when `enabled` is false, the chunk is empty, or the channel
 * count doesn't match what the encoder was constructed with.
 *
 * @param encoder Pointer to the encoder instance.
 * @param chunk Pointer to the audio chunk to encode.
 */
void dop_encoder_encode(dop_encoder_t* encoder, audio_chunk_t* chunk);

/**
 * @brief Free the DoP encoder and its resources.
 *
 * @param encoder Pointer to the encoder instance to free.
 */
void dop_encoder_free(dop_encoder_t* encoder);

/**
 * @brief Check if a carrier rate is supported for DoP encoding.
 *
 * Carrier sample rates that produce a valid DoP stream are DSD64/128/256
 * over the 44.1 kHz and 48 kHz rate families.
 *
 * @param rate The carrier rate to check.
 * @return True if the rate is supported, false otherwise.
 */
bool dop_encoder_is_supported_carrier_rate(int rate);

/**
 * @brief Check if the DoP encoder is enabled.
 *
 * @param encoder Pointer to the encoder instance.
 * @return True if enabled, false otherwise.
 */
bool dop_encoder_is_enabled(const dop_encoder_t* encoder);

#endif  // CLIB_DOP_DOP_ENCODER_H
