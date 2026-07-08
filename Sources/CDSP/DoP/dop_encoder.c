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

#include "dop_encoder.h"

#include "sigma_delta_modulator.h"
#include <stdint.h>
#include <stdlib.h>

/**
 * @brief State for a single DoP encoder channel.
 */
typedef struct {
  /** FIFO buffer for interpolation. Holds 32 * 2 doubles. */
  double fifo[64];
  /** Current position in the FIFO buffer. */
  int fifo_pos;
  /** Alternating DoP marker byte (0x05 or 0xFA). */
  uint8_t marker;
  /** Sigma-delta modulator instance for this channel. */
  sigma_delta_modulator_t* modulator;
} dop_encoder_channel_state_t;

struct dop_encoder {
  /** Number of audio channels. */
  int channels;
  /**
   * True if the encoder is enabled (i.e. constructor was asked to encode
   * AND the carrier rate is supported).
   */
  bool enabled;
  /** Array of channel states. */
  dop_encoder_channel_state_t* channel_states;
  /**
   * Polyphase coefficient table laid out as `coeffs[phase * subFilterTaps +
   * tap]`. Each phase is normalized to unit DC gain.
   */
  double* coeffs;
};

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef ENABLE_ACCELERATE
#include <Accelerate/Accelerate.h>
#endif

#define DOP_ENC_PHASES 16
#define DOP_ENC_REAL_TAPS 511
#define DOP_ENC_NUM_TAPS 512
#define DOP_ENC_SUB_FILTER_TAPS 32
#define DOP_ENC_FIFO_MASK 31

// Carrier sample rates that produce a valid DoP stream — DSD64/128/256
// over the 44.1 kHz and 48 kHz rate families. Anything outside this set
// can't be DoP-encoded: the modulator's filter table only has entries
// for these specific DSD rates, and a downstream DAC won't recognize
// the marker pattern at any other carrier rate.
static const int supported_carrier_rates[] = {176400, 352800, 705600,
                                              192000, 384000, 768000};

bool dop_encoder_is_supported_carrier_rate(int rate) {
  size_t count =
      sizeof(supported_carrier_rates) / sizeof(supported_carrier_rates[0]);
  for (size_t i = 0; i < count; i++) {
    if (supported_carrier_rates[i] == rate) return true;
  }
  return false;
}

/**
 * @brief Computes the modified Bessel function of the first kind of order zero,
 * I0(x).
 *
 * This function uses a power series expansion to approximate I0(x).
 * It is used in the calculation of the Kaiser window.
 *
 * @param x The input value.
 * @return The approximated value of I0(x).
 */
static double bessel_i0_enc(double x) {
  double sum = 1.0;
  double denominator = 1.0;
  double i = 1.0;
  while (i < 25.0) {
    denominator *= i;
    double term = pow(x / 2.0, i) / denominator;
    sum += term * term;
    i += 1.0;
  }
  return sum;
}

/// Build a polyphase decomposition of a 511-tap β=11 Kaiser-windowed
/// sinc with cutoff `cutoffHz / dsdRate`. Phase `p` gets taps
/// `h[m·phases + p]` for `m = 0..<subFilterTaps`; each phase is
/// normalized to unit DC gain so a constant input passes through
/// unchanged.
/**
 * @brief Builds the polyphase coefficient table for the 16x interpolation
 * filter.
 *
 * Designs a 511-tap Kaiser-windowed sinc filter and decomposes it into 16
 * phases (polyphase representation) with 32 taps per phase. Each phase is
 * normalized to ensure unit DC gain, so a constant input passes through
 * unchanged.
 *
 * @param sample_rate The PCM sample rate (carrier rate).
 * @param cutoff_hz The desired cutoff frequency in Hz.
 * @return A pointer to the allocated flat array of polyphase coefficients (size
 * 16 * 32 doubles), or NULL on allocation failure.
 */
static double* build_coeffs(double sample_rate, double cutoff_hz) {
  double beta = 11.0;
  double dsd_rate = sample_rate * 16.0;
  double cutoff = cutoff_hz / dsd_rate;
  double alpha = (double)(DOP_ENC_REAL_TAPS - 1) / 2.0;
  double i0_beta = bessel_i0_enc(beta);

  double taps[DOP_ENC_NUM_TAPS];
  memset(taps, 0, sizeof(taps));
  for (int i = 0; i < DOP_ENC_REAL_TAPS; i++) {
    double t = (double)i - alpha;
    double sinc_val = 0.0;
    if (t == 0.0) {
      sinc_val = 2.0 * cutoff;
    } else {
      double angle = 2.0 * M_PI * cutoff * t;
      sinc_val = sin(angle) / (M_PI * t);
    }
    double widx = sqrt(1.0 - pow(t / alpha, 2.0));
    double window_val = bessel_i0_enc(beta * widx) / i0_beta;
    taps[i] = sinc_val * window_val;
  }

  size_t total_elements = DOP_ENC_PHASES * DOP_ENC_SUB_FILTER_TAPS;
  double* p = (double*)calloc(total_elements, sizeof(double));
  if (!p) return NULL;

  for (int ph = 0; ph < DOP_ENC_PHASES; ph++) {
    double sub_sum = 0.0;
    for (int m = 0; m < DOP_ENC_SUB_FILTER_TAPS; m++) {
      sub_sum += taps[m * DOP_ENC_PHASES + ph];
    }
    double scale = (sub_sum != 0.0) ? (1.0 / sub_sum) : 0.0;
    for (int m = 0; m < DOP_ENC_SUB_FILTER_TAPS; m++) {
      double v = taps[m * DOP_ENC_PHASES + ph] * scale;
      int store_idx =
          ph * DOP_ENC_SUB_FILTER_TAPS + (DOP_ENC_SUB_FILTER_TAPS - 1 - m);
      p[store_idx] = v;
    }
  }
  return p;
}

dop_encoder_t* dop_encoder_create(int channels, double sample_rate,
                                  bool output_dop, sdm_filter_t filter_name,
                                  double cutoff_hz) {
  if (channels <= 0) return NULL;
  dop_encoder_t* enc = (dop_encoder_t*)calloc(1, sizeof(dop_encoder_t));
  if (!enc) return NULL;
  enc->channels = channels;
  enc->coeffs = build_coeffs(sample_rate, cutoff_hz);
  if (!enc->coeffs) {
    free(enc);
    return NULL;
  }

  int rate_int = (int)round(sample_rate);
  bool supported = dop_encoder_is_supported_carrier_rate(rate_int);
  enc->enabled = output_dop && supported;

  if (!enc->enabled) {
    return enc;
  }

  enc->channel_states = (dop_encoder_channel_state_t*)calloc(
      channels, sizeof(dop_encoder_channel_state_t));
  if (!enc->channel_states) {
    free(enc->coeffs);
    free(enc);
    return NULL;
  }

  double dsd_rate = sample_rate * 16.0;
  uint32_t freq = (uint32_t)round(dsd_rate);
  for (int ch = 0; ch < channels; ch++) {
    enc->channel_states[ch].modulator =
        sigma_delta_modulator_create(filter_name, freq);
    enc->channel_states[ch].marker = 0x05;
    if (!enc->channel_states[ch].modulator) {
      for (int i = 0; i < ch; i++) {
        sigma_delta_modulator_free(enc->channel_states[i].modulator);
      }
      free(enc->channel_states);
      free(enc->coeffs);
      free(enc);
      return NULL;
    }
  }
  return enc;
}

/**
 * @brief Encodes a single channel's PCM buffer to DoP in-place.
 *
 * For each input PCM frame, this function:
 * 1. Pushes the sample into a duplicate-history FIFO.
 * 2. Runs a 16-phase polyphase interpolation filter.
 * 3. Feeds each interpolated sample to the Sigma-Delta Modulator (scaled by 0.5
 *    for headroom).
 * 4. Packs the 16 resulting DSD bits into a 16-bit word (MSB to LSB matching
 * the phase order).
 * 5. Combines the DSD word with the alternating DoP marker (0x05 / 0xFA) into a
 *    24-bit integer container.
 * 6. Sign-extends the 24-bit integer to 32-bit and normalizes it to a float
 * [-1.0, 1.0] to overwrite the input buffer.
 *
 * @param state Pointer to the per-channel encoder state.
 * @param buf The audio buffer to process in-place.
 * @param frames Number of frames in the buffer.
 * @param coeffs Polyphase filter coefficients.
 */
static void encode_channel(dop_encoder_channel_state_t* state,
                           mutable_waveform_t buf, size_t frames,
                           const double* coeffs) {
  if (!buf) return;
  double* fifo = state->fifo;
  int pos = state->fifo_pos;
  uint8_t marker = state->marker;
  sigma_delta_modulator_t* mod = state->modulator;

  for (size_t t = 0; t < frames; t++) {
    // Push the new PCM sample into both halves of the polyphase FIR's history.
    // By duplicating the history buffer, we can perform the convolution on a
    // contiguous block of memory without checking for ring buffer wrap-around
    // in the inner loop.
    double sample_val = buf[t];
    fifo[pos] = sample_val;
    fifo[pos + DOP_ENC_SUB_FILTER_TAPS] = sample_val;

    // For each of the 16 oversampled phases, compute the interpolated
    // sample and feed it through the SDM. Phase p=0 is the oldest
    // sample within this frame's 16-sample window and ends up in the
    // MSB of the packed word; phase p=15 is the newest and ends up in
    // the LSB. This matches the bit ordering used by `DoPDecoder`.
    uint16_t word = 0;
    int base_idx = pos + 1;
    for (int p = 0; p < 16; p++) {
      const double* coeff_p = coeffs + p * 32;
      const double* fifo_p = fifo + base_idx;
      double acc = 0.0;
#ifdef ENABLE_ACCELERATE
      // Use Apple's Accelerate framework for optimized dot product if
      // available.
      vDSP_dotprD(coeff_p, 1, fifo_p, 1, &acc, 32);
#else
      for (int m = 0; m < 32; m++) {
        acc += coeff_p[m] * fifo_p[m];
      }
#endif
      // Scale input by 0.5 for SDM headroom.
      double dsd = sigma_delta_modulator_sample(mod, acc * 0.5);
      if (dsd > 0.0) {
        word |= (uint16_t)(1 << (15 - p));
      }
    }

    // 24-bit DoP container: marker in bits 23..16, DSD word in bits 15..0.
    // Sign-extend from int24 and normalize back to ±1.0 float for the
    // playback backend, which will re-quantize to the device format
    // (must be S24 or S32 to preserve the bit pattern).
    uint32_t val24 = ((uint32_t)marker << 16) | (uint32_t)word;
    // Sign-extend 24-bit to 32-bit: shift left by 8, then arithmetic shift
    // right by 8.
    int32_t int_val = (int32_t)(val24 << 8) >> 8;
    buf[t] = (double)int_val / 8388608.0;

    marker = (marker == 0x05) ? 0xFA : 0x05;
    pos = (pos + 1) & DOP_ENC_FIFO_MASK;
  }

  state->fifo_pos = pos;
  state->marker = marker;
}

void dop_encoder_encode(dop_encoder_t* encoder, audio_chunk_t* chunk) {
  if (!encoder || !encoder->enabled || !chunk) return;
  size_t n = audio_chunk_get_valid_frames(chunk);
  if (n == 0 || (int)audio_chunk_get_channels(chunk) != encoder->channels)
    return;
  for (int ch = 0; ch < encoder->channels; ch++) {
    encode_channel(&encoder->channel_states[ch],
                   audio_chunk_get_channel(chunk, ch), n, encoder->coeffs);
  }
}

void dop_encoder_free(dop_encoder_t* encoder) {
  if (!encoder) return;
  if (encoder->channel_states) {
    for (int ch = 0; ch < encoder->channels; ch++) {
      sigma_delta_modulator_free(encoder->channel_states[ch].modulator);
    }
    free(encoder->channel_states);
  }
  if (encoder->coeffs) free(encoder->coeffs);
  free(encoder);
}
