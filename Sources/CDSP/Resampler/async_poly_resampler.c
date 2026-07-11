// Asynchronous polynomial resampler.
//
// Same buffer layout, same `last_index`
// semantics, same `t_ratio` accumulation, same Newton-form polynomial
// formulas — output samples agree bit-for-bit.
//
// No anti-aliasing; for
// quality use `AsyncSincResampler`.
//
// Memory: every internal buffer is sized at init based on `chunkSize` and
// `maxRelativeRatio`. There is **no** dynamic allocation on the hot path.

#include "async_poly_resampler.h"

struct async_poly_resampler {
  size_t channels;
  size_t chunk_size;
  poly_interpolation_t interpolation;
  size_t interpolator_len;  // = nbr_points
  // Ratio bookkeeping.
  double base_ratio;
  double resample_ratio;
  double target_ratio;
  double max_relative_ratio;
  double last_index;  // tracking index
  // Per-channel input buffer. Layout:
  //   [0 .. 2*nbr_points)            — history padding zone
  //   [2*nbr_points .. 2*nbr_points+chunkSize) — current chunk
  audio_buffers_t* input_buffer;
  // Pre-allocated per-frame scratch. `start_idx_scratch` holds the integer
  // floor of `idx`, computed once when `frac_scratch` is built — saving the
  // inner loops a floor() + int cast per output frame.
  int* start_idx_scratch;
  double* frac_scratch;
  size_t max_output_frames;
};

#include <math.h>
#include <stdlib.h>
#include <string.h>

async_poly_resampler_t* async_poly_resampler_create(
    size_t channels, size_t input_rate, size_t output_rate,
    poly_interpolation_t interpolation, size_t chunk_size,
    double max_relative_ratio, config_error_t* err) {
  if (channels == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION, "AsyncPolyResampler: channels must be positive");
    return NULL;
  }
  if (chunk_size == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION, "AsyncPolyResampler: chunk_size must be positive");
    return NULL;
  }
  if (input_rate == 0 || output_rate == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION, "AsyncPolyResampler: rates must be positive");
    return NULL;
  }
  if (max_relative_ratio < 1.0) max_relative_ratio = 1.1;

  async_poly_resampler_t* resampler =
      (async_poly_resampler_t*)calloc(1, sizeof(async_poly_resampler_t));
  if (!resampler) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate AsyncPolyResampler");
    return NULL;
  }

  resampler->channels = channels;
  resampler->chunk_size = chunk_size;
  resampler->interpolation = interpolation;
  resampler->interpolator_len =
      (size_t)poly_interpolation_nbr_points(interpolation);
  resampler->base_ratio = (double)output_rate / (double)input_rate;

  if (chunk_size < 2 * resampler->interpolator_len ||
      chunk_size > SIZE_MAX - 2 * resampler->interpolator_len) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "AsyncPolyResampler: chunk_size %zu is out of bounds for interpolator length %zu",
                     chunk_size, resampler->interpolator_len);
    async_poly_resampler_free(resampler);
    return NULL;
  }

  size_t buf_len = chunk_size + 2 * resampler->interpolator_len;
  resampler->input_buffer = audio_buffers_create(channels, buf_len);
  if (!resampler->input_buffer) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate AsyncPolyResampler input buffer");
    async_poly_resampler_free(resampler);
    return NULL;
  }

  resampler->resample_ratio = resampler->base_ratio;
  resampler->target_ratio = resampler->base_ratio;
  resampler->max_relative_ratio = max_relative_ratio;
  // Set initial index based on number of points.
  resampler->last_index = -((double)resampler->interpolator_len / 2.0);

  /* Compute a conservative upper bound on output frames for the worst-case
     (highest) resampling ratio. This bounds the size of scratch space and
     prevents dynamic reallocation. 'most_neg_last_index' occurs when the
     tracking index starts as far left as possible. */
  double most_neg_last_index = -((double)resampler->interpolator_len / 2.0);
  double max_ratio_abs = resampler->base_ratio * max_relative_ratio;
  double raw_max =
      ((double)chunk_size - (double)(resampler->interpolator_len + 1) -
       most_neg_last_index) *
      max_ratio_abs;

  if (isnan(raw_max) || isinf(raw_max) || raw_max < 0.0 || raw_max > (double)(SIZE_MAX - 32)) {
    config_error_set(err, CONFIG_ERR_VALIDATION, "AsyncPolyResampler: calculated maximum output size is invalid");
    async_poly_resampler_free(resampler);
    return NULL;
  }
  resampler->max_output_frames = (size_t)(ceil(raw_max)) + 16;

  resampler->start_idx_scratch =
      (int*)calloc(resampler->max_output_frames, sizeof(int));
  resampler->frac_scratch =
      (double*)calloc(resampler->max_output_frames, sizeof(double));
  if (!resampler->start_idx_scratch || !resampler->frac_scratch) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate AsyncPolyResampler scratch buffers");
    async_poly_resampler_free(resampler);
    return NULL;
  }

  return resampler;
}

void async_poly_resampler_free(async_poly_resampler_t* resampler) {
  if (!resampler) return;
  if (resampler->input_buffer) audio_buffers_free(resampler->input_buffer);
  free(resampler->start_idx_scratch);
  free(resampler->frac_scratch);
  free(resampler);
}

void async_poly_resampler_set_relative_ratio(async_poly_resampler_t* resampler,
                                             double multiplier) {
  if (!resampler) return;
  if (multiplier < 0.000001) multiplier = 0.000001;
  if (multiplier > resampler->max_relative_ratio) multiplier = resampler->max_relative_ratio;
  resampler->target_ratio = resampler->base_ratio * multiplier;
}

double async_poly_resampler_get_ratio(const async_poly_resampler_t* resampler) {
  return resampler ? resampler->resample_ratio : 1.0;
}

size_t async_poly_resampler_get_max_output_frames(
    const async_poly_resampler_t* resampler) {
  return resampler ? resampler->max_output_frames : 0;
}

size_t async_poly_resampler_get_chunk_size(
    const async_poly_resampler_t* resampler) {
  return resampler ? resampler->chunk_size : 0;
}

size_t async_poly_resampler_get_channels(
    const async_poly_resampler_t* resampler) {
  return resampler ? resampler->channels : 0;
}

/**
 * @brief Computes the number of output frames expected for the current block.
 *
 * It uses the average ratio between current and target resampling ratios,
 * and accounts for the current index offset (`last_index`) and filter group
 * delay.
 *
 * @param resampler Pointer to the resampler instance.
 * @return Estimated number of output frames.
 */
static inline size_t get_next_output_frames(
    const async_poly_resampler_t* resampler) {
  // Calculate output size for input
  // — `.floor()`, not `.ceil()`.
  double avg_ratio =
      0.5 * resampler->resample_ratio + 0.5 * resampler->target_ratio;
  double raw =
      ((double)resampler->chunk_size -
       (double)(resampler->interpolator_len + 1) - resampler->last_index) *
      avg_ratio;
  return (size_t)floor(raw);
}

/**
 * @brief Performs linear polynomial interpolation on input buffer.
 *
 * Loops over channels and output frames, interpolating between the two nearest
 * samples.
 *
 * @param resampler Pointer to the resampler instance.
 * @param output_frames Number of output frames to generate.
 * @param output Output chunk to store interpolated samples.
 */
static void run_linear(async_poly_resampler_t* resampler, size_t output_frames,
                       audio_chunk_t* output) {
  size_t n_len = resampler->interpolator_len;
  size_t two_n_len = 2 * n_len;
  const int* idx_buf = resampler->start_idx_scratch;
  const double* frac_buf = resampler->frac_scratch;

  for (size_t ch = 0; ch < resampler->channels; ch++) {
    const double* buf = audio_buffers_get_channel(resampler->input_buffer, ch);
    double* out = audio_chunk_get_channel(output, ch);
    if (!buf || !out) continue;
    for (size_t frame = 0; frame < output_frames; frame++) {
      double x = frac_buf[frame];
      const double* base = buf + idx_buf[frame] + two_n_len;
      double y0 = base[0];
      double y1 = base[1];
      out[frame] = y0 + x * (y1 - y0);
    }
  }
}

/**
 * @brief Performs cubic polynomial interpolation (4-point Lagrange/Newton
 * form).
 *
 * @param resampler Pointer to the resampler instance.
 * @param output_frames Number of output frames to generate.
 * @param output Output chunk to store interpolated samples.
 */
static void run_cubic(async_poly_resampler_t* resampler, size_t output_frames,
                      audio_chunk_t* output) {
  size_t n_len = resampler->interpolator_len;  // 4
  size_t base_offset = 2 * n_len - 1;          // 7
  const int* idx_buf = resampler->start_idx_scratch;
  const double* frac_buf = resampler->frac_scratch;

  for (size_t ch = 0; ch < resampler->channels; ch++) {
    const double* buf = audio_buffers_get_channel(resampler->input_buffer, ch);
    double* out = audio_chunk_get_channel(output, ch);
    if (!buf || !out) continue;
    for (size_t frame = 0; frame < output_frames; frame++) {
      double x = frac_buf[frame];
      const double* base = buf + idx_buf[frame] + base_offset;
      double y0 = base[0];
      double y1 = base[1];
      double y2 = base[2];
      double y3 = base[3];

      double a0 = y1;
      double a1 = -1.0 / 3.0 * y0 - 0.5 * y1 + y2 - 1.0 / 6.0 * y3;
      double a2 = 0.5 * (y0 + y2) - y1;
      double a3 = 0.5 * (y1 - y2) + 1.0 / 6.0 * (y3 - y0);

      out[frame] = a0 + x * (a1 + x * (a2 + x * a3));
    }
  }
}

/**
 * @brief Performs quintic polynomial interpolation (6-point Lagrange/Newton
 * form).
 *
 * @param resampler Pointer to the resampler instance.
 * @param output_frames Number of output frames to generate.
 * @param output Output chunk to store interpolated samples.
 */
static void run_quintic(async_poly_resampler_t* resampler, size_t output_frames,
                        audio_chunk_t* output) {
  size_t n_len = resampler->interpolator_len;  // 6
  size_t base_offset = 2 * n_len - 2;          // 10
  const int* idx_buf = resampler->start_idx_scratch;
  const double* frac_buf = resampler->frac_scratch;

  for (size_t ch = 0; ch < resampler->channels; ch++) {
    const double* buf = audio_buffers_get_channel(resampler->input_buffer, ch);
    double* out = audio_chunk_get_channel(output, ch);
    if (!buf || !out) continue;
    for (size_t frame = 0; frame < output_frames; frame++) {
      double x = frac_buf[frame];
      const double* base = buf + idx_buf[frame] + base_offset;
      double a = base[0];
      double b = base[1];
      double c = base[2];
      double d = base[3];
      double e = base[4];
      double f = base[5];

      double k5 = -a + 5.0 * b - 10.0 * c + 10.0 * d - 5.0 * e + f;
      double k4 = 5.0 * a - 20.0 * b + 30.0 * c - 20.0 * d + 5.0 * e;
      double k3 = -5.0 * a - 5.0 * b + 50.0 * c - 70.0 * d + 35.0 * e - 5.0 * f;
      double k2 = -5.0 * a + 80.0 * b - 150.0 * c + 80.0 * d - 5.0 * e;
      double k1 =
          6.0 * a - 60.0 * b - 40.0 * c + 120.0 * d - 30.0 * e + 4.0 * f;
      double k0 = 120.0 * c;

      double x2 = x * x;
      double x3 = x2 * x;
      double x4 = x2 * x2;
      double x5 = x2 * x3;
      double val = k5 * x5 + k4 * x4 + k3 * x3 + k2 * x2 + k1 * x + k0;

      out[frame] = (1.0 / 120.0) * val;
    }
  }
}

/**
 * @brief Performs septic polynomial interpolation (8-point Lagrange/Newton
 * form).
 *
 * Uses Horner's method to evaluate the polynomial coefficients efficiently.
 *
 * @param resampler Pointer to the resampler instance.
 * @param output_frames Number of output frames to generate.
 * @param output Output chunk to store interpolated samples.
 */
static void run_septic(async_poly_resampler_t* resampler, size_t output_frames,
                       audio_chunk_t* output) {
  size_t n_len = resampler->interpolator_len;  // 8
  size_t base_offset = 2 * n_len - 3;          // 13
  const int* idx_buf = resampler->start_idx_scratch;
  const double* frac_buf = resampler->frac_scratch;

  for (size_t ch = 0; ch < resampler->channels; ch++) {
    const double* buf = audio_buffers_get_channel(resampler->input_buffer, ch);
    double* out = audio_chunk_get_channel(output, ch);
    if (!buf || !out) continue;
    for (size_t frame = 0; frame < output_frames; frame++) {
      double x = frac_buf[frame];
      const double* base = buf + idx_buf[frame] + base_offset;
      double a = base[0];
      double b = base[1];
      double c = base[2];
      double d = base[3];
      double e = base[4];
      double f = base[5];
      double g = base[6];
      double h = base[7];

      double k7 = -a + 7.0 * b - 21.0 * c + 35.0 * d - 35.0 * e + 21.0 * f -
                  7.0 * g + h;
      double k6 = 7.0 * a - 42.0 * b + 105.0 * c - 140.0 * d + 105.0 * e -
                  42.0 * f + 7.0 * g;
      double k5 = -7.0 * a - 14.0 * b + 189.0 * c - 490.0 * d + 595.0 * e -
                  378.0 * f + 119.0 * g - 14.0 * h;
      double k4 = -35.0 * a + 420.0 * b - 1365.0 * c + 1960.0 * d - 1365.0 * e +
                  420.0 * f - 35.0 * g;
      double k3 = 56.0 * a - 497.0 * b + 336.0 * c + 1715.0 * d - 3080.0 * e +
                  1869.0 * f - 448.0 * g + 49.0 * h;
      double k2 = 28.0 * a - 378.0 * b + 3780.0 * c - 6860.0 * d + 3780.0 * e -
                  378.0 * f + 28.0 * g;
      double k1 = -48.0 * a + 504.0 * b - 3024.0 * c - 1260.0 * d + 5040.0 * e -
                  1512.0 * f + 336.0 * g - 36.0 * h;
      double k0 = 5040.0 * d;

      // Horner's method
      double val =
          k0 +
          x * (k1 +
               x * (k2 + x * (k3 + x * (k4 + x * (k5 + x * (k6 + x * k7))))));

      out[frame] = (1.0 / 5040.0) * val;
    }
  }
}

resampler_error_t async_poly_resampler_process(
    async_poly_resampler_t* resampler, const audio_chunk_t* input,
    audio_chunk_t* output) {
  if (!resampler || !input || !output) return RESAMPLER_ERR_INVALID_PARAMETER;
  if (audio_chunk_get_valid_frames(input) != resampler->chunk_size) {
    return RESAMPLER_ERR_INPUT_SIZE_MISMATCH;
  }
  if (audio_chunk_get_channels(input) != resampler->channels) {
    return RESAMPLER_ERR_CHANNEL_COUNT_MISMATCH;
  }
  if (audio_chunk_get_channels(output) != resampler->channels) {
    return RESAMPLER_ERR_CHANNEL_COUNT_MISMATCH;
  }
  size_t output_frames = get_next_output_frames(resampler);
  if (output_frames == 0) {
    resampler->last_index -= (double)resampler->chunk_size;
    double min_safe_idx = -2.0 * (double)resampler->interpolator_len;
    if (resampler->last_index < min_safe_idx) {
      resampler->last_index = min_safe_idx;
    }
    resampler->resample_ratio = resampler->target_ratio;
    audio_chunk_set_valid_frames(output, 0);
    return RESAMPLER_OK;
  }
  if (audio_chunk_get_frames(output) < output_frames) {
    return RESAMPLER_ERR_OUTPUT_BUFFER_TOO_SMALL;
  }

  size_t n_len = resampler->interpolator_len;
  size_t two_n_len = 2 * n_len;

  // Shift buffer + write new chunk wait-free and crash-safe.
  /* Shift the historical samples from the end of the previous chunk (of length
     2 * n_len) to the beginning of the buffer. This history buffer is necessary
     for interpolators requiring lookback (e.g. cubic or quintic interpolation
     look at historical points). */
  for (size_t ch = 0; ch < resampler->channels; ch++) {
    double* base = audio_buffers_get_channel(resampler->input_buffer, ch);
    if (!base) continue;
    memmove(base, base + resampler->chunk_size, two_n_len * sizeof(double));
  }
  for (size_t ch = 0; ch < resampler->channels; ch++) {
    const double* src_ptr = audio_chunk_get_channel(input, ch);
    double* dst_ptr = audio_buffers_get_channel(resampler->input_buffer, ch);
    if (!src_ptr || !dst_ptr) continue;
    memcpy(dst_ptr + two_n_len, src_ptr,
           resampler->chunk_size * sizeof(double));
  }

  /* Pre-compute idx and frac per output frame.
     Interpolate/ramp the time increments smoothly across the block.
     t_ratio is the current sample step duration. We step through the block,
     updating the virtual time index `idx` by the current `t_ratio`. To optimize
     inner channel loops, we pre-calculate integer offsets (start_idx_scratch)
     and fractional phases (frac_scratch), avoiding redundant floor() and
     casting in the channel loops. */
  double t_ratio_start = 1.0 / resampler->resample_ratio;
  double t_ratio_end = 1.0 / resampler->target_ratio;
  double t_ratio_increment =
      (t_ratio_end - t_ratio_start) / (double)output_frames;

  double t_ratio = t_ratio_start;
  double idx = resampler->last_index;
  for (size_t frame = 0; frame < output_frames; frame++) {
    t_ratio += t_ratio_increment;
    idx += t_ratio;
    double idx_floor = floor(idx);
    resampler->start_idx_scratch[frame] = (int)idx_floor;
    resampler->frac_scratch[frame] = idx - idx_floor;
  }
  double final_idx = idx;

  switch (resampler->interpolation) {
    case POLY_INTERPOLATION_LINEAR:
      run_linear(resampler, output_frames, output);
      break;
    case POLY_INTERPOLATION_CUBIC:
      run_cubic(resampler, output_frames, output);
      break;
    case POLY_INTERPOLATION_QUINTIC:
      run_quintic(resampler, output_frames, output);
      break;
    case POLY_INTERPOLATION_SEPTIC:
      run_septic(resampler, output_frames, output);
      break;
    default:
      run_cubic(resampler, output_frames, output);
      break;
  }

  resampler->last_index = final_idx - (double)resampler->chunk_size;
  double min_safe_idx = -2.0 * (double)resampler->interpolator_len;
  if (resampler->last_index < min_safe_idx) {
    resampler->last_index = min_safe_idx;
  }
  resampler->resample_ratio = resampler->target_ratio;
  audio_chunk_set_valid_frames(output, output_frames);
  return RESAMPLER_OK;
}
