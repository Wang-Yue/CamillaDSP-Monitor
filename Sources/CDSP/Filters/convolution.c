#include "convolution.h"

#include "FFT/real_fft.h"

struct convolution_filter {
  char name[64];
  size_t chunk_size;
  size_t num_segments;
  real_fft_t* fft;
  double** spec_re;
  double** spec_im;
  double** hist_re;
  double** hist_im;
  size_t write_idx;
  double* overlap_buffer;
  double* time_buf;
  double* spec_accum_re;
  double* spec_accum_im;
};

typedef double double4 __attribute__((vector_size(32), aligned(8)));

static inline double4 load_double4(const double* p) {
  return *(const double4*)p;
}

static inline void store_double4(double* p, double4 v) { *(double4*)p = v; }

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Config/engine_config_types.h"

// Uniform-partitioned overlap-save FIR convolution.
// Stockham-style segmented overlap-save with one 2N-point real FFT per
// chunk and an N+1-bin spectrum-domain multiply-accumulate across the
// segment history.
//
//   - Uses `RealFFT`, which stores the same N+1 unique bins as separate
//     `specRe`/`specIm` arrays. The flat layout (DC at index 0, Nyquist
//     at index N, both with `im == 0`) lets us run the spectrum
//     multiply through `vDSP_zvmulD` / `vDSP_zvmaD` without any DC/
//     Nyquist special-casing.
//   - `RealFFT.inverse` produces `length · signal`. The inverse does not
//     scale, so we pre-divide coefficients by
//     `2 * data_length` to compensate.
//   - All hot-path buffers are owned by raw `UnsafeMutablePointer`s
//     (`AudioBuffers`-style) so `process(waveform:)` cannot trip
//     Swift's Array CoW path that a `[PrcFmt]` field would.

/// Build a convolution filter from raw IR samples.
///
/// - Parameters:
///   - coefficients: Impulse response, in time-domain sample order.
///     Must be non-empty.
///   - chunkSize: Per-call block length `N`. Must match the
///     `validFrames` the pipeline will hand to `process`.
///
/// Resolve the parameters to a flat IR buffer. Only called from the
/// control plane (filter creation / hot-swap), never from
/// `process(waveform:)`.
///
/// Convenience initialiser that resolves `ConvParameters` to a flat
/// IR buffer first (control plane only, may touch the filesystem).
/**
 * @brief Helper to get the size in bytes of a binary sample format.
 *
 * @param format The binary sample format.
 * @return The size in bytes, or 0 if invalid.
 */
static size_t get_raw_sample_size(binary_sample_format_t format) {
  switch (format) {
    case BINARY_SAMPLE_FORMAT_S16_LE:
      return 2;
    case BINARY_SAMPLE_FORMAT_S24_3_LE:
      return 3;
    case BINARY_SAMPLE_FORMAT_S32_LE:
      return 4;
    case BINARY_SAMPLE_FORMAT_F32_LE:
      return 4;
    case BINARY_SAMPLE_FORMAT_F64_LE:
      return 8;
    default:
      return 0;
  }
}

/**
 * @brief Loads a single channel from a WAV file and converts it to double.
 *
 * Supports 16-bit, 24-bit, 32-bit PCM and float, and 64-bit float formats.
 *
 * @param path Path to the WAV file.
 * @param channel The channel index to load (0-indexed).
 * @param out_count Output pointer to store the number of loaded samples.
 * @return Pointer to the allocated double array containing samples, or NULL on
 * failure.
 */
static double* load_wav_file(const char* path, int channel, size_t* out_count) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;

  uint8_t header[44];
  if (fread(header, 1, 44, f) != 44) {
    fclose(f);
    return NULL;
  }

  if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0 ||
      memcmp(header + 12, "fmt ", 4) != 0) {
    fclose(f);
    return NULL;
  }

  uint16_t audio_format = header[20] | (header[21] << 8);
  uint16_t channels = header[22] | (header[23] << 8);
  uint16_t bits_per_sample = header[34] | (header[35] << 8);

  if (audio_format != 1 && audio_format != 3) {
    fclose(f);
    return NULL;
  }

  if (channel < 0 || channel >= (int)channels) {
    fclose(f);
    return NULL;
  }

  // Find data chunk
  uint32_t data_bytes = 0;
  if (memcmp(header + 36, "data", 4) == 0) {
    data_bytes = header[40] | (header[41] << 8) | (header[42] << 16) |
                 (header[43] << 24);
  } else {
    fseek(f, 36, SEEK_SET);
    uint8_t chunk_id[4];
    uint32_t chunk_size;
    while (fread(chunk_id, 1, 4, f) == 4) {
      if (fread(&chunk_size, 4, 1, f) != 1) break;
      if (memcmp(chunk_id, "data", 4) == 0) {
        data_bytes = chunk_size;
        break;
      }
      fseek(f, chunk_size, SEEK_CUR);
    }
  }

  if (data_bytes == 0) {
    fclose(f);
    return NULL;
  }

  size_t bytes_per_sample = bits_per_sample / 8;
  if (channels == 0 || bytes_per_sample == 0) {
    fclose(f);
    return NULL;
  }
  size_t num_frames = data_bytes / (channels * bytes_per_sample);
  if (num_frames == 0) {
    fclose(f);
    return NULL;
  }

  double* result = (double*)malloc(num_frames * sizeof(double));
  if (!result) {
    fclose(f);
    return NULL;
  }

  uint8_t* frame_buf = (uint8_t*)malloc(channels * bytes_per_sample);
  if (!frame_buf) {
    free(result);
    fclose(f);
    return NULL;
  }

  size_t read_frames = 0;
  for (size_t i = 0; i < num_frames; i++) {
    if (fread(frame_buf, 1, channels * bytes_per_sample, f) !=
        channels * bytes_per_sample) {
      break;
    }
    const uint8_t* src = frame_buf + channel * bytes_per_sample;
    double sample = 0.0;
    if (bits_per_sample == 16) {
      int16_t val = src[0] | (src[1] << 8);
      sample = (double)val / 32768.0;
    } else if (bits_per_sample == 24) {
      int32_t val = src[0] | (src[1] << 8) | (src[2] << 16);
      if (val & 0x800000) val |= ~0xFFFFFF;
      sample = (double)val / 8388608.0;
    } else if (bits_per_sample == 32) {
      if (audio_format == 3) {
        float val;
        memcpy(&val, src, 4);
        sample = (double)val;
      } else {
        int32_t val = src[0] | (src[1] << 8) | (src[2] << 16) | (src[3] << 24);
        sample = (double)val / 2147483648.0;
      }
    } else if (bits_per_sample == 64) {
      double val;
      memcpy(&val, src, 8);
      sample = val;
    }
    result[read_frames++] = sample;
  }

  free(frame_buf);
  fclose(f);
  *out_count = read_frames;
  return result;
}

/**
 * @brief Loads raw PCM or text data from a file and converts it to double.
 *
 * For "TEXT" format, reads line by line. For binary formats, reads according
 * to specified sample format.
 *
 * @param path Path to the raw file.
 * @param format_str Format string (e.g., "S16_LE", "TEXT").
 * @param skip_bytes Number of bytes (or lines for TEXT) to skip at start.
 * @param read_bytes Max bytes (or lines for TEXT) to read.
 * @param out_count Output pointer to store the number of loaded samples.
 * @return Pointer to the allocated double array containing samples, or NULL on
 * failure.
 */
static double* load_raw_file(const char* path, const char* format_str,
                             int skip_bytes, int read_bytes,
                             size_t* out_count) {
  if (strcmp(format_str, "TEXT") == 0) {
    FILE* f = fopen(path, "r");
    if (!f) return NULL;
    char line[128];
    bool skip_failed = false;
    for (int i = 0; i < skip_bytes; i++) {
      if (!fgets(line, sizeof(line), f)) {
        skip_failed = true;
        break;
      }
    }
    if (skip_failed) {
      fclose(f);
      return NULL;
    }
    size_t cap = 1024;
    double* result = (double*)malloc(cap * sizeof(double));
    if (!result) {
      fclose(f);
      return NULL;
    }
    size_t count = 0;
    while (fgets(line, sizeof(line), f)) {
      if (read_bytes > 0 && (int)count >= read_bytes) break;
      if (count >= cap) {
        cap *= 2;
        double* new_res = (double*)realloc(result, cap * sizeof(double));
        if (!new_res) {
          free(result);
          fclose(f);
          return NULL;
        }
        result = new_res;
      }
      char* endptr;
      double val = strtod(line, &endptr);
      if (endptr != line) {
        result[count++] = val;
      }
    }
    fclose(f);
    *out_count = count;
    return result;
  }

  FILE* f = fopen(path, "rb");
  if (!f) return NULL;

  if (skip_bytes > 0) {
    fseek(f, skip_bytes, SEEK_SET);
  }

  binary_sample_format_t format = binary_sample_format_from_string(format_str);
  if (format == BINARY_SAMPLE_FORMAT_INVALID) {
    fclose(f);
    return NULL;
  }

  size_t sample_size = get_raw_sample_size(format);
  if (sample_size == 0) {
    fclose(f);
    return NULL;
  }

  fseek(f, 0, SEEK_END);
  long file_size = ftell(f) - skip_bytes;
  fseek(f, skip_bytes, SEEK_SET);
  if (file_size <= 0) {
    fclose(f);
    return NULL;
  }

  long max_read = file_size;
  if (read_bytes > 0 && read_bytes < file_size) {
    max_read = read_bytes;
  }

  size_t num_samples = max_read / sample_size;
  if (num_samples == 0) {
    fclose(f);
    return NULL;
  }

  double* result = (double*)malloc(num_samples * sizeof(double));
  if (!result) {
    fclose(f);
    return NULL;
  }

  uint8_t* buf = (uint8_t*)malloc(sample_size);
  if (!buf) {
    free(result);
    fclose(f);
    return NULL;
  }

  size_t read_count = 0;
  for (size_t i = 0; i < num_samples; i++) {
    if (fread(buf, 1, sample_size, f) != sample_size) {
      break;
    }
    double val = 0.0;
    switch (format) {
      case BINARY_SAMPLE_FORMAT_S16_LE: {
        int16_t v = buf[0] | (buf[1] << 8);
        val = (double)v / 32767.0;
        break;
      }
      case BINARY_SAMPLE_FORMAT_S24_3_LE: {
        int32_t v = buf[0] | (buf[1] << 8) | (buf[2] << 16);
        if (v & 0x800000) v |= ~0xFFFFFF;
        val = (double)v / 8388607.0;
        break;
      }
      case BINARY_SAMPLE_FORMAT_S32_LE: {
        int32_t v = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
        val = (double)v / 2147483647.0;
        break;
      }
      case BINARY_SAMPLE_FORMAT_F32_LE: {
        float v;
        memcpy(&v, buf, 4);
        val = (double)v;
        break;
      }
      case BINARY_SAMPLE_FORMAT_F64_LE: {
        double v;
        memcpy(&v, buf, 8);
        val = v;
        break;
      }
      default:
        break;
    }
    result[read_count++] = val;
  }

  free(buf);
  fclose(f);
  *out_count = read_count;
  return result;
}

convolution_filter_t* convolution_filter_create(const char* name,
                                                const conv_parameters_t* params,
                                                size_t chunk_size,
                                                config_error_t* err) {
  if (!params) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Convolution params is NULL");
    return NULL;
  }
  if (chunk_size == 0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Convolution chunk_size must be positive");
    return NULL;
  }
  convolution_filter_t* filter =
      (convolution_filter_t*)calloc(1, sizeof(convolution_filter_t));
  if (!filter) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate convolution filter wrapper");
    return NULL;
  }
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "convolution");
  }
  filter->chunk_size = chunk_size;
  size_t fft_len = 2 * chunk_size;
  filter->fft = real_fft_create(fft_len, err);
  if (!filter->fft) {
    free(filter);
    return NULL;
  }
  size_t spec_len = real_fft_get_spectrum_length(filter->fft);

  const double* coeffs = NULL;
  size_t coeffs_count = 0;
  double* dummy_coeffs = NULL;
  double* scratch = NULL;

  if (params->type == CONV_TYPE_VALUES) {
    coeffs = params->values;
    coeffs_count = params->values_count;
  } else if (params->type == CONV_TYPE_DUMMY) {
    size_t len = params->length > 0 ? params->length : 1;
    dummy_coeffs = (double*)calloc(len, sizeof(double));
    if (!dummy_coeffs) {
      goto fail;
    }
    dummy_coeffs[0] = 1.0;
    coeffs = dummy_coeffs;
    coeffs_count = len;
  } else if (params->type == CONV_TYPE_WAV) {
    size_t count = 0;
    dummy_coeffs = load_wav_file(params->filename, params->channel, &count);
    coeffs = dummy_coeffs;
    coeffs_count = count;
  } else if (params->type == CONV_TYPE_RAW) {
    size_t count = 0;
    dummy_coeffs = load_raw_file(params->filename, params->format,
                                 params->skip_bytes_lines,
                                 params->read_bytes_lines, &count);
    coeffs = dummy_coeffs;
    coeffs_count = count;
  }

  if (!coeffs || coeffs_count == 0) {
    goto fail;
  }

  size_t num_seg = (coeffs_count + chunk_size - 1) / chunk_size;
  filter->num_segments = num_seg;
  filter->spec_re = (double**)calloc(num_seg, sizeof(double*));
  filter->spec_im = (double**)calloc(num_seg, sizeof(double*));
  filter->hist_re = (double**)calloc(num_seg, sizeof(double*));
  filter->hist_im = (double**)calloc(num_seg, sizeof(double*));

  if (!filter->spec_re || !filter->spec_im || !filter->hist_re || !filter->hist_im) {
    goto fail;
  }

  scratch = (double*)calloc(fft_len, sizeof(double));
  if (!scratch) {
    goto fail;
  }
  double inv_scale = 1.0 / (double)fft_len;

  // Pre-scale and FFT each IR segment into split-complex spectrum storage.
  for (size_t s = 0; s < num_seg; s++) {
    filter->spec_re[s] = (double*)calloc(spec_len, sizeof(double));
    filter->spec_im[s] = (double*)calloc(spec_len, sizeof(double));
    filter->hist_re[s] = (double*)calloc(spec_len, sizeof(double));
    filter->hist_im[s] = (double*)calloc(spec_len, sizeof(double));

    if (!filter->spec_re[s] || !filter->spec_im[s] || !filter->hist_re[s] || !filter->hist_im[s]) {
      goto fail;
    }

    memset(scratch, 0, fft_len * sizeof(double));
    size_t offset = s * chunk_size;
    size_t copy_len = (coeffs_count > offset) ? (coeffs_count - offset) : 0;
    if (copy_len > chunk_size) copy_len = chunk_size;
    if (copy_len > 0) {
      // Scale-and-copy into the first half; zero the rest.
      for (size_t k = 0; k < copy_len; k++) {
        scratch[k] = coeffs[offset + k] * inv_scale;
      }
    }
    real_fft_forward(filter->fft, scratch, filter->spec_re[s],
                     filter->spec_im[s]);
  }
  free(scratch);
  scratch = NULL;
  if (dummy_coeffs) {
    free(dummy_coeffs);
    dummy_coeffs = NULL;
  }

  filter->write_idx = 0;
  filter->overlap_buffer = (double*)calloc(chunk_size, sizeof(double));
  filter->time_buf = (double*)calloc(fft_len, sizeof(double));
  filter->spec_accum_re = (double*)calloc(spec_len, sizeof(double));
  filter->spec_accum_im = (double*)calloc(spec_len, sizeof(double));

  if (!filter->overlap_buffer || !filter->time_buf || !filter->spec_accum_re || !filter->spec_accum_im) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate convolution scratch buffers");
    goto fail;
  }

  return filter;

fail:
  if (err && err->type == CONFIG_ERR_NONE) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Failed to initialize convolution filter '%s' (check IR values or file format/existence)",
                     name ? name : "");
  }
  if (dummy_coeffs) free(dummy_coeffs);
  if (scratch) free(scratch);
  convolution_filter_free(filter);
  return NULL;
}

/**
 * @brief Processes one chunk of audio data using partitioned overlap-add
 * convolution.
 *
 * This function performs the following steps:
 * 1. Copies the input block to the time buffer and pads with zeros.
 * 2. Computes the forward FFT of the padded block and stores it in the history
 * buffer.
 * 3. Performs frequency-domain multiply-accumulate with the partitioned IR
 * segments.
 * 4. Computes the inverse FFT of the accumulated spectrum.
 * 5. Reconstructs the output block using overlap-add.
 *
 * @param filter Pointer to the convolution filter.
 * @param waveform In-place buffer containing the input block, which will be
 * overwritten with the output.
 */
static void process_chunk(convolution_filter_t* filter,
                          mutable_waveform_t waveform) {
  if (!filter || filter->num_segments == 0) return;
  size_t cs = filter->chunk_size;
  size_t spec_len = real_fft_get_spectrum_length(filter->fft);
  size_t num_seg = filter->num_segments;
  size_t widx = filter->write_idx;

  // 1. Stage the new block in the first `chunkSize` samples of
  //    `inputBuf` (`time_buf`); zero the second half (the FFT zero-pad).
  memcpy(filter->time_buf, waveform, cs * sizeof(double));
  memset(filter->time_buf + cs, 0, cs * sizeof(double));

  // 2. Advance the history index and FFT the new block into that
  //    slot. The slot now holds the spectrum of `inputBuf` (`time_buf`).
  real_fft_forward(filter->fft, filter->time_buf, filter->hist_re[widx],
                   filter->hist_im[widx]);

  memset(filter->spec_accum_re, 0, spec_len * sizeof(double));
  memset(filter->spec_accum_im, 0, spec_len * sizeof(double));

  // 3. Spectrum-domain multiply-accumulate across the segment
  //    history. seg=0 pairs the newest input with coeff[0]; seg=k
  //    pairs the input from `k` blocks ago with coeff[k].
  for (size_t s = 0; s < num_seg; s++) {
    size_t hidx = (widx + num_seg - s) % num_seg;
    const double* hre = filter->hist_re[hidx];
    const double* him = filter->hist_im[hidx];
    const double* sre = filter->spec_re[s];
    const double* sim = filter->spec_im[s];
    double* acc_re = filter->spec_accum_re;
    double* acc_im = filter->spec_accum_im;

    size_t vec_len = (spec_len / 16) * 16;
    for (size_t k = 0; k < vec_len; k += 16) {
      double4 h_re0 = load_double4(&hre[k]);
      double4 h_im0 = load_double4(&him[k]);
      double4 s_re0 = load_double4(&sre[k]);
      double4 s_im0 = load_double4(&sim[k]);
      double4 a_re0 = load_double4(&acc_re[k]);
      double4 a_im0 = load_double4(&acc_im[k]);

      double4 h_re1 = load_double4(&hre[k + 4]);
      double4 h_im1 = load_double4(&him[k + 4]);
      double4 s_re1 = load_double4(&sre[k + 4]);
      double4 s_im1 = load_double4(&sim[k + 4]);
      double4 a_re1 = load_double4(&acc_re[k + 4]);
      double4 a_im1 = load_double4(&acc_im[k + 4]);

      double4 h_re2 = load_double4(&hre[k + 8]);
      double4 h_im2 = load_double4(&him[k + 8]);
      double4 s_re2 = load_double4(&sre[k + 8]);
      double4 s_im2 = load_double4(&sim[k + 8]);
      double4 a_re2 = load_double4(&acc_re[k + 8]);
      double4 a_im2 = load_double4(&acc_im[k + 8]);

      double4 h_re3 = load_double4(&hre[k + 12]);
      double4 h_im3 = load_double4(&him[k + 12]);
      double4 s_re3 = load_double4(&sre[k + 12]);
      double4 s_im3 = load_double4(&sim[k + 12]);
      double4 a_re3 = load_double4(&acc_re[k + 12]);
      double4 a_im3 = load_double4(&acc_im[k + 12]);

      a_re0 += h_re0 * s_re0 - h_im0 * s_im0;
      a_im0 += h_re0 * s_im0 + h_im0 * s_re0;

      a_re1 += h_re1 * s_re1 - h_im1 * s_im1;
      a_im1 += h_re1 * s_im1 + h_im1 * s_re1;

      a_re2 += h_re2 * s_re2 - h_im2 * s_im2;
      a_im2 += h_re2 * s_im2 + h_im2 * s_re2;

      a_re3 += h_re3 * s_re3 - h_im3 * s_im3;
      a_im3 += h_re3 * s_im3 + h_im3 * s_re3;

      store_double4(&acc_re[k], a_re0);
      store_double4(&acc_im[k], a_im0);
      store_double4(&acc_re[k + 4], a_re1);
      store_double4(&acc_im[k + 4], a_im1);
      store_double4(&acc_re[k + 8], a_re2);
      store_double4(&acc_im[k + 8], a_im2);
      store_double4(&acc_re[k + 12], a_re3);
      store_double4(&acc_im[k + 12], a_im3);
    }
    for (size_t k = vec_len; k < spec_len; k++) {
      acc_re[k] += hre[k] * sre[k] - him[k] * sim[k];
      acc_im[k] += hre[k] * sim[k] + him[k] * sre[k];
    }
  }

  // 4. Inverse FFT. RealFFT.inverse multiplies by
  //    `length = 2N`, but `coeffsF` was pre-divided by `2N` in init,
  //    so the net result is the un-normalised linear convolution
  //    sum.
  real_fft_inverse(filter->fft, filter->spec_accum_re, filter->spec_accum_im,
                   filter->time_buf);

  // 5. Overlap-add output: out[i] = ifft[i] + overlap_prev[i] for
  //    i in 0..<N; overlap_next = ifft[N..2N].
  for (size_t i = 0; i < cs; i++) {
    waveform[i] = filter->time_buf[i] + filter->overlap_buffer[i];
  }
  memcpy(filter->overlap_buffer, filter->time_buf + cs, cs * sizeof(double));

  filter->write_idx = (widx + 1) % num_seg;
}

/// Process one block in-place. The hot path is allocation-free in
/// steady state; everything below is pointer arithmetic over the
/// preallocated storage from `init`.
void convolution_filter_process(convolution_filter_t* filter,
                                mutable_waveform_t waveform, size_t count) {
  if (!filter || !waveform || count == 0) return;
  size_t cs = filter->chunk_size;
  size_t processed = 0;
  while (processed + cs <= count) {
    process_chunk(filter, waveform + processed);
    processed += cs;
  }
}

void convolution_filter_free(convolution_filter_t* filter) {
  if (!filter) return;
  if (filter->fft) {
    size_t num_seg = filter->num_segments;
    for (size_t s = 0; s < num_seg; s++) {
      if (filter->spec_re && filter->spec_re[s]) free(filter->spec_re[s]);
      if (filter->spec_im && filter->spec_im[s]) free(filter->spec_im[s]);
      if (filter->hist_re && filter->hist_re[s]) free(filter->hist_re[s]);
      if (filter->hist_im && filter->hist_im[s]) free(filter->hist_im[s]);
    }
    if (filter->spec_re) free(filter->spec_re);
    if (filter->spec_im) free(filter->spec_im);
    if (filter->hist_re) free(filter->hist_re);
    if (filter->hist_im) free(filter->hist_im);
    real_fft_free(filter->fft);
  }
  if (filter->overlap_buffer) free(filter->overlap_buffer);
  if (filter->time_buf) free(filter->time_buf);
  if (filter->spec_accum_re) free(filter->spec_accum_re);
  if (filter->spec_accum_im) free(filter->spec_accum_im);
  free(filter);
}
