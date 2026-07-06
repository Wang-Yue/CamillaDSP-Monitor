#ifndef CLIB_DOP_DOP_ENCODER_H
#define CLIB_DOP_DOP_ENCODER_H

// PCM → DoP encoder. Inverse of `DoPDecoder`: converts a chunk of PCM
// audio at the carrier rate into DSD-over-PCM, in place. For each input
// frame we
//   1. interpolate 16× to the DSD rate using a 511-tap β=11 Kaiser-windowed
//      polyphase sinc (same shape as the decoder, normalized per phase
//      for unit DC gain),
//   2. modulate the oversampled signal with a per-channel sigma-delta
//      modulator (using the configured `SDMFilter`, defaulting to `sdm-6`), and
//   3. pack the 16 resulting DSD bits into the lower 16 bits of a 24-bit
//      container, with an alternating `0x05` / `0xFA` marker in the
//      upper byte.
//
// The encoded chunk satisfies the strict-alternation detection state
// machine in `DoPDecoder` and round-trips through any DAC that natively
// understands DoP. To preserve the bit pattern through CoreAudio the
// playback format must be S24 or S32 (F32 will quantize the marker
// away); the encoder itself just emits float-normalised 24-bit values
// and trusts the playback backend to forward them losslessly.
//
// SDM state per channel is carried by an embedded `SigmaDeltaModulator`;
// the polyphase coefficient table is shared across channels and built
// once at init.

#include "Audio/audio_chunk.h"
#include "Config/engine_config_types.h"
#include "sigma_delta_modulator.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double fifo[64]; // 32 * 2 doubles
    int fifo_pos;
    uint8_t marker;
    sigma_delta_modulator_t* modulator;
} dop_encoder_channel_state_t;

/// PCM → DoP encoder. Inverse of `DoPDecoder`: converts a chunk of PCM
/// audio at the carrier rate into DSD-over-PCM, in place. For each input
/// frame we
///   1. interpolate 16× to the DSD rate using a 511-tap β=11 Kaiser-windowed
///      polyphase sinc (same shape as the decoder, normalized per phase
///      for unit DC gain),
///   2. modulate the oversampled signal with a per-channel sigma-delta
///      modulator (using the configured `SDMFilter`, defaulting to `sdm-6`), and
///   3. pack the 16 resulting DSD bits into the lower 16 bits of a 24-bit
///      container, with an alternating `0x05` / `0xFA` marker in the
///      upper byte.
///
/// The encoded chunk satisfies the strict-alternation detection state
/// machine in `DoPDecoder` and round-trips through any DAC that natively
/// understands DoP. To preserve the bit pattern through CoreAudio the
/// playback format must be S24 or S32 (F32 will quantize the marker
/// away); the encoder itself just emits float-normalised 24-bit values
/// and trusts the playback backend to forward them losslessly.
///
/// SDM state per channel is carried by an embedded `SigmaDeltaModulator`;
/// the polyphase coefficient table is shared across channels and built
/// once at init.
typedef struct {
    int channels;
    /// `true` iff the constructor was asked to encode AND the carrier rate
    /// is in `supportedCarrierRates`. `encode(...)` is an unconditional
    /// no-op when this is `false`.
    bool enabled;
    dop_encoder_channel_state_t* channel_states;
    /// Polyphase coefficient table laid out as `coeffs[phase * subFilterTaps + tap]`.
    /// Each phase is normalized to unit DC gain; with a constant input sequence
    /// the interpolated output equals the input value, so the SDM input scale
    /// matches the PCM input scale. Built unconditionally — at unsupported
    /// rates the table is harmless dead weight (~4 KB) but keeping the
    /// allocation unconditional simplifies the deinit path.
    double* coeffs; // 16 * 32 doubles = 512 doubles
} dop_encoder_t;

/// Construct an encoder. Always succeeds, but only actually encodes
/// when `output_dop` is `true` *and* `sample_rate` is one of
/// `supportedCarrierRates`. The mismatched case reduces `encode(...)` to a no-op.
///
/// - Parameters:
///   - channels: Number of audio channels.
///   - sample_rate: The PCM sample rate (carrier rate).
///   - output_dop: If true, enables DoP encoding.
///   - filter_name: Noise-shaper filter name (defaults to `.sdm6`).
///   - cutoff_hz: Passband cutoff of the interpolation filter (default 20 kHz).
///     Lower values trade ultrasonic passband for sharper image rejection. Ignored when `enabled` is false.
dop_encoder_t* dop_encoder_create(int channels, double sample_rate, bool output_dop, sdm_filter_t filter_name, double cutoff_hz);
/// Encode the chunk's `validFrames` PCM samples into DoP, in place.
/// No-op when `enabled` is `false`, the chunk is empty, or the channel
/// count doesn't match what the encoder was constructed with.
void dop_encoder_encode(dop_encoder_t* encoder, audio_chunk_t* chunk);
void dop_encoder_free(dop_encoder_t* encoder);

/// Carrier sample rates that produce a valid DoP stream — DSD64/128/256
/// over the 44.1 kHz and 48 kHz rate families. Anything outside this set
/// can't be DoP-encoded: the modulator's filter table only has entries
/// for these specific DSD rates, and a downstream DAC won't recognize
/// the marker pattern at any other carrier rate.
bool dop_encoder_is_supported_carrier_rate(int rate);

#ifdef __cplusplus
}
#endif

#endif // CLIB_DOP_DOP_ENCODER_H
