#ifndef CLIB_DOP_DOP_DECODER_H
#define CLIB_DOP_DOP_DECODER_H

// DoP detection and decoding.
//
// DSD-over-PCM packs 16 1-bit DSD samples into the lower 16 bits of each
// PCM frame; the upper byte carries a magic marker that alternates
// `0x05` ↔ `0xFA` between consecutive frames. We detect by looking for that
// strict alternation and decode by streaming the recovered DSD bytes
// through the same 511-tap Kaiser-windowed sinc the previous
// `DSDPolyphaseDecimator` used (β=11, cutoff = 20 kHz / dsd_rate),
// resampling 16:1 back to the carrier rate.
//
// The detection state machine is hysteretic: 32 consecutive valid alternating
// frames per channel to lock on, 64 consecutive bad frames to release. The
// asymmetry kills the PCM↔DSD flicker the previous "reset on a single bad
// frame" code exhibited at chunk boundaries and around isolated bit errors.
//
// The hot path runs on the audio thread, so the decoder allocates nothing
// per call. Per-channel state is a 64-byte ring FIFO of DSD bytes; the
// convolution becomes 64 byte-indexed table lookups
// (`acc += ctables[i][fifo[i]]`) — each table precomputes the contribution
// of a byte at a given offset in the filter, replacing the per-bit
// conditional add. Filter shape, tap count, and cutoff are unchanged from
// the previous design, so the SINAD numbers the existing tests pin down
// across DSD64 / 128 / 256 at 44.1 / 48 kHz families are preserved.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Audio/audio_chunk.h"

/// Per-channel state for DoP decoding. Holds a 64-byte ring FIFO of DSD bytes
/// and hysteretic lock counters.
typedef struct {
  int consec_valid;
  int consec_invalid;
  bool is_active;
  uint8_t last_marker;
  bool is_32bit_container;
  bool container_known;
  uint8_t fifo[64];
  int fifo_pos;
} dop_decoder_channel_state_t;

/// DoP detection and decoding engine.
///
/// DSD-over-PCM packs 16 1-bit DSD samples into the lower 16 bits of each
/// PCM frame; the upper byte carries a magic marker that alternates
/// `0x05` ↔ `0xFA` between consecutive frames. We detect by looking for that
/// strict alternation and decode by streaming the recovered DSD bytes
/// through the same 511-tap Kaiser-windowed sinc the previous
/// `DSDPolyphaseDecimator` used (β=11, cutoff = 20 kHz / dsd_rate),
/// resampling 16:1 back to the carrier rate.
///
/// The detection state machine is hysteretic: 32 consecutive valid alternating
/// frames per channel to lock on, 64 consecutive bad frames to release. The
/// asymmetry kills the PCM↔DSD flicker the previous "reset on a single bad
/// frame" code exhibited at chunk boundaries and around isolated bit errors.
///
/// The hot path runs on the audio thread, so the decoder allocates nothing
/// per call. Per-channel state is a 64-byte ring FIFO of DSD bytes; the
/// convolution becomes 64 byte-indexed table lookups
/// (`acc += ctables[i][fifo[i]]`) — each table precomputes the contribution
/// of a byte at a given offset in the filter, replacing the per-bit
/// conditional add. Filter shape, tap count, and cutoff are unchanged from
/// the previous design, so the SINAD numbers the existing tests pin down
/// across DSD64 / 128 / 256 at 44.1 / 48 kHz families are preserved.
typedef struct {
  int channels;
  bool bypass_dop;
  dop_decoder_channel_state_t* channel_states;
  /// Flat ctable storage: `ctables[i*256 + b]` is the convolution
  /// contribution of byte `b` placed at table index `i`. Built once at
  /// init from the configured sample rate and cutoff; never resized.
  double* ctables;  // 64 * 256 doubles
  bool is_dop_active;

  bool logged_active;
  bool last_seen_active;
  int chunks_at_seen_state;
} dop_decoder_t;

/// Create DoP decoder.
/// - Parameters:
///   - channels: Number of audio channels.
///   - sample_rate: The PCM sample rate (carrier rate).
///   - bypass_dop: If true, DoP detection is disabled and input is passed
///   through.
///   - cutoff_hz: Passband cutoff of the post-DSD lowpass (default 20 kHz).
///     Lower values trade ultrasonic passband for higher SINAD.
dop_decoder_t* dop_decoder_create(int channels, double sample_rate,
                                  bool bypass_dop, double cutoff_hz);
/// Detect DoP and (when active) decode the chunk in place. Returns
/// `true` iff the chunk was decoded.
bool dop_decoder_detect_and_process(dop_decoder_t* decoder,
                                    audio_chunk_t* chunk);
void dop_decoder_free(dop_decoder_t* decoder);

#endif  // CLIB_DOP_DOP_DECODER_H
