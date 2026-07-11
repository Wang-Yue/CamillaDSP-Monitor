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
#if defined(ENABLE_BLAS)
#include <cblas.h>
#elif defined(ENABLE_ACCELERATE)
#include <Accelerate/Accelerate.h>
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef double double4 __attribute__((vector_size(32)));

static inline double4 load_double4(const double* p) {
  double4 v;
  memcpy(&v, p, sizeof(double4));
  return v;
}

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
#if defined(ENABLE_BLAS)
  // Scratch buffers for batched BLAS processing
  double* padded_buf;
  double* scratch_X;
  double* scratch_Y;
  size_t scratch_capacity;
#endif
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

#include "../Audio/double_helpers.h"

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
  double i0_beta = double_bessel_i0(beta);

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
    double window_val = double_bessel_i0(beta * widx) / i0_beta;
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
    dop_encoder_free(enc);
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
    dop_encoder_free(enc);
    return NULL;
  }

  double dsd_rate = sample_rate * 16.0;
  uint32_t freq = (uint32_t)round(dsd_rate);
  for (int ch = 0; ch < channels; ch++) {
    enc->channel_states[ch].modulator =
        sigma_delta_modulator_create(filter_name, freq);
    enc->channel_states[ch].marker = 0x05;
    if (!enc->channel_states[ch].modulator) {
      dop_encoder_free(enc);
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
#if defined(ENABLE_BLAS)
static bool ensure_scratch_capacity(dop_encoder_channel_state_t* state,
                                    size_t capacity) {
  if (state->scratch_capacity >= capacity) return true;

  double* p =
      (double*)realloc(state->padded_buf, (32 + capacity) * sizeof(double));
  if (!p) return false;
  state->padded_buf = p;

  p = (double*)realloc(state->scratch_X, capacity * 32 * sizeof(double));
  if (!p) return false;
  state->scratch_X = p;

  p = (double*)realloc(state->scratch_Y, capacity * 16 * sizeof(double));
  if (!p) return false;
  state->scratch_Y = p;

  state->scratch_capacity = capacity;
  return true;
}

static void encode_channel(dop_encoder_channel_state_t* state,
                           mutable_waveform_t buf, size_t frames,
                           const double* coeffs);

static void encode_channel_batched(dop_encoder_channel_state_t* state,
                                   mutable_waveform_t buf, size_t frames,
                                   const double* coeffs) {
  if (!buf || frames == 0) return;
  if (!ensure_scratch_capacity(state, frames)) {
    encode_channel(state, buf, frames, coeffs);
    return;
  }

  double* padded_buf = state->padded_buf;
  double* X = state->scratch_X;
  double* Y = state->scratch_Y;

  // Copy history from FIFO
  memcpy(padded_buf, state->fifo + state->fifo_pos, 32 * sizeof(double));

  // Copy current frames
  memcpy(padded_buf + 32, buf, frames * sizeof(double));

  // Populate X matrix
  for (size_t t = 0; t < frames; t++) {
    memcpy(X + t * 32, padded_buf + t + 1, 32 * sizeof(double));
  }

  // Perform matrix multiplication
  cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasTrans, (int)frames, 16, 32, 1.0,
              X, 32, coeffs, 32, 0.0, Y, 16);

  uint8_t marker = state->marker;
  sigma_delta_modulator_t* mod = state->modulator;
  for (size_t t = 0; t < frames; t++) {
    uint16_t word = 0;
    for (int p = 0; p < 16; p++) {
      double acc = Y[t * 16 + p];
      double dsd = sigma_delta_modulator_sample(mod, acc * 0.5);
      if (dsd > 0.0) {
        word |= (uint16_t)(1 << (15 - p));
      }
    }

    uint32_t val24 = ((uint32_t)marker << 16) | (uint32_t)word;
    int32_t int_val = (val24 & 0x800000) ? (int32_t)(val24 | 0xFF000000) : (int32_t)val24;
    buf[t] = (double)int_val / 8388608.0;

    marker = (marker == 0x05) ? 0xFA : 0x05;
  }

  // Update history in FIFO
  memcpy(state->fifo, padded_buf + frames, 32 * sizeof(double));
  memcpy(state->fifo + 32, padded_buf + frames, 32 * sizeof(double));
  state->fifo_pos = 0;
  state->marker = marker;
}
#endif

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
    const double* fifo_p = fifo + base_idx;
#if defined(ENABLE_BLAS)
    double acc[16];
    cblas_dgemv(CblasRowMajor, CblasNoTrans, 16, 32, 1.0, coeffs, 32, fifo_p, 1,
                0.0, acc, 1);
    for (int p = 0; p < 16; p++) {
      double dsd = sigma_delta_modulator_sample(mod, acc[p] * 0.5);
      if (dsd > 0.0) {
        word |= (uint16_t)(1 << (15 - p));
      }
    }
#else
    double4 f0 = load_double4(fifo_p);
    double4 f1 = load_double4(fifo_p + 4);
    double4 f2 = load_double4(fifo_p + 8);
    double4 f3 = load_double4(fifo_p + 12);
    double4 f4 = load_double4(fifo_p + 16);
    double4 f5 = load_double4(fifo_p + 20);
    double4 f6 = load_double4(fifo_p + 24);
    double4 f7 = load_double4(fifo_p + 28);

    for (int p = 0; p < 16; p += 4) {
      const double* coeff_p0 = coeffs + p * 32;
      const double* coeff_p1 = coeffs + (p + 1) * 32;
      const double* coeff_p2 = coeffs + (p + 2) * 32;
      const double* coeff_p3 = coeffs + (p + 3) * 32;

      double4 c0_0 = load_double4(coeff_p0);
      double4 c0_1 = load_double4(coeff_p0 + 4);
      double4 c0_2 = load_double4(coeff_p0 + 8);
      double4 c0_3 = load_double4(coeff_p0 + 12);
      double4 c0_4 = load_double4(coeff_p0 + 16);
      double4 c0_5 = load_double4(coeff_p0 + 20);
      double4 c0_6 = load_double4(coeff_p0 + 24);
      double4 c0_7 = load_double4(coeff_p0 + 28);

      double4 c1_0 = load_double4(coeff_p1);
      double4 c1_1 = load_double4(coeff_p1 + 4);
      double4 c1_2 = load_double4(coeff_p1 + 8);
      double4 c1_3 = load_double4(coeff_p1 + 12);
      double4 c1_4 = load_double4(coeff_p1 + 16);
      double4 c1_5 = load_double4(coeff_p1 + 20);
      double4 c1_6 = load_double4(coeff_p1 + 24);
      double4 c1_7 = load_double4(coeff_p1 + 28);

      double4 c2_0 = load_double4(coeff_p2);
      double4 c2_1 = load_double4(coeff_p2 + 4);
      double4 c2_2 = load_double4(coeff_p2 + 8);
      double4 c2_3 = load_double4(coeff_p2 + 12);
      double4 c2_4 = load_double4(coeff_p2 + 16);
      double4 c2_5 = load_double4(coeff_p2 + 20);
      double4 c2_6 = load_double4(coeff_p2 + 24);
      double4 c2_7 = load_double4(coeff_p2 + 28);

      double4 c3_0 = load_double4(coeff_p3);
      double4 c3_1 = load_double4(coeff_p3 + 4);
      double4 c3_2 = load_double4(coeff_p3 + 8);
      double4 c3_3 = load_double4(coeff_p3 + 12);
      double4 c3_4 = load_double4(coeff_p3 + 16);
      double4 c3_5 = load_double4(coeff_p3 + 20);
      double4 c3_6 = load_double4(coeff_p3 + 24);
      double4 c3_7 = load_double4(coeff_p3 + 28);

      double4 sum0 = c0_0 * f0;
      double4 sum1 = c1_0 * f0;
      double4 sum2 = c2_0 * f0;
      double4 sum3 = c3_0 * f0;

      sum0 += c0_1 * f1;
      sum1 += c1_1 * f1;
      sum2 += c2_1 * f1;
      sum3 += c3_1 * f1;

      sum0 += c0_2 * f2;
      sum1 += c1_2 * f2;
      sum2 += c2_2 * f2;
      sum3 += c3_2 * f2;

      sum0 += c0_3 * f3;
      sum1 += c1_3 * f3;
      sum2 += c2_3 * f3;
      sum3 += c3_3 * f3;

      sum0 += c0_4 * f4;
      sum1 += c1_4 * f4;
      sum2 += c2_4 * f4;
      sum3 += c3_4 * f4;

      sum0 += c0_5 * f5;
      sum1 += c1_5 * f5;
      sum2 += c2_5 * f5;
      sum3 += c3_5 * f5;

      sum0 += c0_6 * f6;
      sum1 += c1_6 * f6;
      sum2 += c2_6 * f6;
      sum3 += c3_6 * f6;

      sum0 += c0_7 * f7;
      sum1 += c1_7 * f7;
      sum2 += c2_7 * f7;
      sum3 += c3_7 * f7;

      double acc0 = sum0[0] + sum0[1] + sum0[2] + sum0[3];
      double acc1 = sum1[0] + sum1[1] + sum1[2] + sum1[3];
      double acc2 = sum2[0] + sum2[1] + sum2[2] + sum2[3];
      double acc3 = sum3[0] + sum3[1] + sum3[2] + sum3[3];

      if (sigma_delta_modulator_sample(mod, acc0 * 0.5) > 0.0) {
        word |= (uint16_t)(1 << (15 - p));
      }
      if (sigma_delta_modulator_sample(mod, acc1 * 0.5) > 0.0) {
        word |= (uint16_t)(1 << (15 - (p + 1)));
      }
      if (sigma_delta_modulator_sample(mod, acc2 * 0.5) > 0.0) {
        word |= (uint16_t)(1 << (15 - (p + 2)));
      }
      if (sigma_delta_modulator_sample(mod, acc3 * 0.5) > 0.0) {
        word |= (uint16_t)(1 << (15 - (p + 3)));
      }
    }
#endif

    // 24-bit DoP container: marker in bits 23..16, DSD word in bits 15..0.
    // Sign-extend from int24 and normalize back to ±1.0 float for the
    // playback backend, which will re-quantize to the device format
    // (must be S24 or S32 to preserve the bit pattern).
    uint32_t val24 = ((uint32_t)marker << 16) | (uint32_t)word;
    // Sign-extend 24-bit to 32-bit: shift left by 8, then arithmetic shift
    // right by 8.
    int32_t int_val = (val24 & 0x800000) ? (int32_t)(val24 | 0xFF000000) : (int32_t)val24;
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
#if defined(ENABLE_BLAS)
    encode_channel_batched(&encoder->channel_states[ch],
                           audio_chunk_get_channel(chunk, ch), n,
                           encoder->coeffs);
#else
    encode_channel(&encoder->channel_states[ch],
                   audio_chunk_get_channel(chunk, ch), n, encoder->coeffs);
#endif
  }
}

void dop_encoder_free(dop_encoder_t* encoder) {
  if (!encoder) return;
  if (encoder->channel_states) {
    for (int ch = 0; ch < encoder->channels; ch++) {
      sigma_delta_modulator_free(encoder->channel_states[ch].modulator);
#if defined(ENABLE_BLAS)
      if (encoder->channel_states[ch].padded_buf)
        free(encoder->channel_states[ch].padded_buf);
      if (encoder->channel_states[ch].scratch_X)
        free(encoder->channel_states[ch].scratch_X);
      if (encoder->channel_states[ch].scratch_Y)
        free(encoder->channel_states[ch].scratch_Y);
#endif
    }
    free(encoder->channel_states);
  }
  if (encoder->coeffs) free(encoder->coeffs);
  free(encoder);
}

bool dop_encoder_is_enabled(const dop_encoder_t* encoder) {
  return encoder ? encoder->enabled : false;
}
