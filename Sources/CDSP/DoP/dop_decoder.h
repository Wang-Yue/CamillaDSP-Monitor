#ifndef CLIB_DOP_DOP_DECODER_H
#define CLIB_DOP_DOP_DECODER_H

/**
 * @file dop_decoder.h
 * @brief DoP (DSD-over-PCM) detection and decoding.
 *
 * DSD-over-PCM packs 16 1-bit DSD samples into the lower 16 bits of each
 * PCM frame; the upper byte carries a magic marker that alternates
 * `0x05` <-> `0xFA` between consecutive frames. We detect by looking for that
 * strict alternation and decode by streaming the recovered DSD bytes
 * through the same 511-tap Kaiser-windowed sinc the previous
 * `DSDPolyphaseDecimator` used (beta=11, cutoff = 20 kHz / dsd_rate),
 * resampling 16:1 back to the carrier rate.
 *
 * The detection state machine is hysteretic: 32 consecutive valid alternating
 * frames per channel to lock on, 64 consecutive bad frames to release. The
 * asymmetry kills the PCM <-> DSD flicker the previous "reset on a single bad
 * frame" code exhibited at chunk boundaries and around isolated bit errors.
 *
 * The hot path runs on the audio thread, so the decoder allocates nothing
 * per call. Per-channel state is a 64-byte ring FIFO of DSD bytes; the
 * convolution becomes 64 byte-indexed table lookups
 * (`acc += ctables[i][fifo[i]]`) - each table precomputes the contribution
 * of a byte at a given offset in the filter, replacing the per-bit
 * conditional add. Filter shape, tap count, and cutoff are unchanged from
 * the previous design, so the SINAD numbers the existing tests pin down
 * across DSD64 / 128 / 256 at 44.1 / 48 kHz families are preserved.
 */

#include <stdbool.h>

#include "Audio/audio_chunk.h"

/**
 * @brief DoP detection and decoding engine.
 */
typedef struct dop_decoder dop_decoder_t;

/**
 * @brief Create a DoP decoder instance.
 *
 * @param channels Number of audio channels.
 * @param sample_rate The PCM sample rate (carrier rate).
 * @param bypass_dop If true, DoP detection is disabled and input is passed
 * through.
 * @param cutoff_hz Passband cutoff of the post-DSD lowpass (typically 20000.0).
 *                  Lower values trade ultrasonic passband for higher SINAD.
 * @return Pointer to the allocated dop_decoder_t instance, or NULL on failure.
 */
dop_decoder_t* dop_decoder_create(int channels, double sample_rate,
                                  bool bypass_dop, double cutoff_hz);

/**
 * @brief Detect DoP and (when active) decode the chunk in place.
 *
 * @param decoder Pointer to the DoP decoder.
 * @param chunk Pointer to the audio chunk to process.
 * @return True if the chunk was decoded as DoP, false if processed as PCM.
 */
bool dop_decoder_detect_and_process(dop_decoder_t* decoder,
                                    audio_chunk_t* chunk);

/**
 * @brief Check if DoP is globally active (any channel has lock).
 *
 * @param decoder Pointer to the DoP decoder.
 * @return True if DoP is active, false otherwise.
 */
bool dop_decoder_is_active(const dop_decoder_t* decoder);

/**
 * @brief Free the DoP decoder instance and its resources.
 *
 * @param decoder Pointer to the DoP decoder to free.
 */
void dop_decoder_free(dop_decoder_t* decoder);

#endif  // CLIB_DOP_DOP_DECODER_H
