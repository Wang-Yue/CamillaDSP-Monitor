// Asynchronous windowed-sinc resampler.
//
// Same buffer layout, same `last_index` semantics, same `t_ratio` accumulation,
// same kernel decimation — output samples agree bit-for-bit (modulo the
// FMA-reduction order in the dot product, which is on the order of a few ULPs).
//
// Memory: every internal buffer is sized at init based on `chunkSize` and
// `maxRelativeRatio`. There is **no** dynamic allocation on the hot path.

#include "async_sinc_resampler.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

async_sinc_resampler_t* async_sinc_resampler_create(
    size_t channels, size_t input_rate, size_t output_rate, size_t sinc_len,
    size_t oversampling_factor, sinc_interpolation_type_t interpolation,
    window_function_t window, double f_cutoff, bool has_f_cutoff,
    size_t chunk_size, double max_relative_ratio) {
  if (channels == 0 || chunk_size == 0 || input_rate == 0 || output_rate == 0)
    return NULL;
  if (max_relative_ratio < 1.0) max_relative_ratio = 1.1;
  if (chunk_size < 2 * sinc_len) return NULL;

  async_sinc_resampler_t* resampler =
      (async_sinc_resampler_t*)calloc(1, sizeof(async_sinc_resampler_t));
  if (!resampler) return NULL;

  resampler->channels = channels;
  resampler->chunk_size = chunk_size;
  resampler->sinc_len = sinc_len;
  resampler->oversampling_factor = oversampling_factor;
  resampler->interpolation = interpolation;
  resampler->base_ratio = (double)output_rate / (double)input_rate;

  // Cutoff: computed as f32 then converted to f64 inside
  // `make_sincs` (`asynchro_sinc.rs:96`). Down-sampling scales the cutoff
  // by the ratio so the kernel doesn't pass aliased high frequencies.
  float base_cutoff =
      has_f_cutoff ? (float)f_cutoff : calculate_cutoff_f32(sinc_len, window);
  float fc_f32 = resampler->base_ratio >= 1.0
                     ? base_cutoff
                     : base_cutoff * (float)resampler->base_ratio;
  double fc = (double)fc_f32;

  resampler->sinc_table =
      make_sinc_table(sinc_len, oversampling_factor, window, fc);
  if (!resampler->sinc_table) {
    free(resampler);
    return NULL;
  }

  // Input buffer sized to: chunkSize + 2*sincLen. Initial
  // contents are zeros — the first chunk's "history" is silence.
  size_t buf_len = chunk_size + 2 * sinc_len;
  resampler->input_buffer = audio_buffers_create(channels, buf_len);
  if (!resampler->input_buffer) {
    free(resampler->sinc_table);
    free(resampler);
    return NULL;
  }

  // Initial state.
  resampler->resample_ratio = resampler->base_ratio;
  resampler->target_ratio = resampler->base_ratio;
  resampler->last_index = -((double)sinc_len - 1.0);

  // Worst-case output frames: minimum lastIndex (= initial value) × maximum
  // possible ratio (= baseRatio × maxRelativeRatio). +16 slack for the
  // ceil() boundary plus future safety.
  double most_neg_last_index = -((double)sinc_len - 1.0);
  double max_ratio_abs = resampler->base_ratio * max_relative_ratio;
  double raw_max =
      ((double)chunk_size - (double)(sinc_len + 1) - most_neg_last_index) *
      max_ratio_abs;
  resampler->max_output_frames = (size_t)(ceil(raw_max)) + 16;

  // Pre-allocate scratch for per-frame state.
  resampler->idx_scratch =
      (double*)calloc(resampler->max_output_frames, sizeof(double));
  resampler->frac_scratch =
      (double*)calloc(resampler->max_output_frames, sizeof(double));
  if (!resampler->idx_scratch || !resampler->frac_scratch) {
    audio_buffers_free(resampler->input_buffer);
    free(resampler->sinc_table);
    free(resampler->idx_scratch);
    free(resampler->frac_scratch);
    free(resampler);
    return NULL;
  }

  return resampler;
}

async_sinc_resampler_t* async_sinc_resampler_create_from_profile(
    size_t channels, size_t input_rate, size_t output_rate,
    resampler_profile_t profile, size_t chunk_size, double max_relative_ratio) {
  size_t sinc_len = 192;
  size_t oversampling_factor = 512;
  window_function_t window = WINDOW_FUNCTION_BLACKMAN_HARRIS2;
  sinc_interpolation_type_t interpolation = SINC_INTERPOLATION_QUADRATIC;

  switch (profile) {
    case RESAMPLER_PROFILE_VERY_FAST:
      sinc_len = 64;
      oversampling_factor = 1024;
      window = WINDOW_FUNCTION_HANN2;
      interpolation = SINC_INTERPOLATION_LINEAR;
      break;
    case RESAMPLER_PROFILE_FAST:
      sinc_len = 128;
      oversampling_factor = 1024;
      window = WINDOW_FUNCTION_BLACKMAN2;
      interpolation = SINC_INTERPOLATION_LINEAR;
      break;
    case RESAMPLER_PROFILE_BALANCED:
      sinc_len = 192;
      oversampling_factor = 512;
      window = WINDOW_FUNCTION_BLACKMAN_HARRIS2;
      interpolation = SINC_INTERPOLATION_QUADRATIC;
      break;
    case RESAMPLER_PROFILE_ACCURATE:
      sinc_len = 256;
      oversampling_factor = 256;
      window = WINDOW_FUNCTION_BLACKMAN_HARRIS2;
      interpolation = SINC_INTERPOLATION_CUBIC;
      break;
    default:
      break;
  }

  return async_sinc_resampler_create(
      channels, input_rate, output_rate, sinc_len, oversampling_factor,
      interpolation, window, 0.0, false, chunk_size, max_relative_ratio);
}

void async_sinc_resampler_free(async_sinc_resampler_t* resampler) {
  if (!resampler) return;
  if (resampler->input_buffer) audio_buffers_free(resampler->input_buffer);
  free(resampler->sinc_table);
  free(resampler->idx_scratch);
  free(resampler->frac_scratch);
  free(resampler);
}

void async_sinc_resampler_set_relative_ratio(async_sinc_resampler_t* resampler,
                                             double multiplier) {
  if (!resampler) return;
  resampler->target_ratio = resampler->base_ratio * multiplier;
}

double async_sinc_resampler_get_ratio(const async_sinc_resampler_t* resampler) {
  return resampler ? resampler->resample_ratio : 1.0;
}

size_t async_sinc_resampler_get_max_output_frames(
    const async_sinc_resampler_t* resampler) {
  return resampler ? resampler->max_output_frames : 0;
}

size_t async_sinc_resampler_get_chunk_size(
    const async_sinc_resampler_t* resampler) {
  return resampler ? resampler->chunk_size : 0;
}

size_t async_sinc_resampler_get_channels(
    const async_sinc_resampler_t* resampler) {
  return resampler ? resampler->channels : 0;
}

static inline size_t get_next_output_frames(
    const async_sinc_resampler_t* resampler) {
  // Calculate output size for input
  // — note `.floor()`, not `.ceil()`. Using ceil
  // here was the source of the off-by-one frame discrepancy.
  double avg_ratio =
      0.5 * resampler->resample_ratio + 0.5 * resampler->target_ratio;
  double raw = ((double)resampler->chunk_size -
                (double)(resampler->sinc_len + 1) - resampler->last_index) *
               avg_ratio;
  return (size_t)floor(raw);
}

typedef struct {
  int idx;
  int sub;
} adjust_point_t;

/// Fetch the (index, subindex) pair for a given (start, frac, sub) triple.
/// Wrap-around logic.
static inline adjust_point_t adjust_point(int start, int frac, int sub,
                                          int factor) {
  int index = start;
  int subindex = frac + sub;
  if (subindex < 0) {
    subindex += factor;
    index -= 1;
  } else if (subindex >= factor) {
    subindex -= factor;
    index += 1;
  }
  return (adjust_point_t){index, subindex};
}

static void run_nearest(async_sinc_resampler_t* resampler, size_t output_frames,
                        audio_chunk_t* output) {
  size_t s_len = resampler->sinc_len;
  size_t two_s_len = 2 * s_len;
  int factor = (int)resampler->oversampling_factor;
  double factor_d = (double)factor;
  const double* table = resampler->sinc_table;
  const double* idx_buf = resampler->idx_scratch;

  for (size_t ch = 0; ch < resampler->channels; ch++) {
    const double* buf = audio_buffers_get_channel(resampler->input_buffer, ch);
    double* out = audio_chunk_get_channel(output, ch);
    if (!buf || !out) continue;
    for (size_t frame = 0; frame < output_frames; frame++) {
      double idx = idx_buf[frame];
      double idx_floor = floor(idx);
      int start_idx = (int)idx_floor;
      int subindex = (int)round((idx - idx_floor) * factor_d);
      if (subindex >= factor) {
        subindex -= factor;
        start_idx += 1;
      }

      double y = sinc_dot_product(buf + start_idx + two_s_len,
                                  table + subindex * s_len, s_len);
      out[frame] = y;
    }
  }
}

static void run_cubic(async_sinc_resampler_t* resampler, size_t output_frames,
                      audio_chunk_t* output) {
  size_t s_len = resampler->sinc_len;
  size_t two_s_len = 2 * s_len;
  int factor = (int)resampler->oversampling_factor;
  double factor_d = (double)factor;
  const double* table = resampler->sinc_table;
  const double* idx_buf = resampler->idx_scratch;
  const double* frac_buf = resampler->frac_scratch;

  for (size_t ch = 0; ch < resampler->channels; ch++) {
    const double* buf = audio_buffers_get_channel(resampler->input_buffer, ch);
    double* out = audio_chunk_get_channel(output, ch);
    if (!buf || !out) continue;
    for (size_t frame = 0; frame < output_frames; frame++) {
      double idx = idx_buf[frame];
      double idx_floor = floor(idx);
      int start_idx = (int)idx_floor;
      int frac = (int)floor((idx - idx_floor) * factor_d);
      double frac_offset = frac_buf[frame];

      // 4 (idx, sub) pairs at sub = -1, 0, 1, 2.
      adjust_point_t p0t = adjust_point(start_idx, frac, -1, factor);
      adjust_point_t p1t = adjust_point(start_idx, frac, 0, factor);
      adjust_point_t p2t = adjust_point(start_idx, frac, 1, factor);
      adjust_point_t p3t = adjust_point(start_idx, frac, 2, factor);

      double p0 = sinc_dot_product(buf + p0t.idx + two_s_len,
                                   table + p0t.sub * s_len, s_len);
      double p1 = sinc_dot_product(buf + p1t.idx + two_s_len,
                                   table + p1t.sub * s_len, s_len);
      double p2 = sinc_dot_product(buf + p2t.idx + two_s_len,
                                   table + p2t.sub * s_len, s_len);
      double p3 = sinc_dot_product(buf + p3t.idx + two_s_len,
                                   table + p3t.sub * s_len, s_len);

      // interp_cubic (asynchro_sinc.rs:118-128).
      double a0 = p1;
      double a1 = -1.0 / 3.0 * p0 - 0.5 * p1 + p2 - 1.0 / 6.0 * p3;
      double a2 = 0.5 * (p0 + p2) - p1;
      double a3 = 0.5 * (p1 - p2) + 1.0 / 6.0 * (p3 - p0);
      double x = frac_offset;
      double x2 = x * x;
      double x3 = x2 * x;
      out[frame] = a0 + a1 * x + a2 * x2 + a3 * x3;
    }
  }
}

static void run_quadratic(async_sinc_resampler_t* resampler,
                          size_t output_frames, audio_chunk_t* output) {
  size_t s_len = resampler->sinc_len;
  size_t two_s_len = 2 * s_len;
  int factor = (int)resampler->oversampling_factor;
  double factor_d = (double)factor;
  const double* table = resampler->sinc_table;
  const double* idx_buf = resampler->idx_scratch;
  const double* frac_buf = resampler->frac_scratch;

  for (size_t ch = 0; ch < resampler->channels; ch++) {
    const double* buf = audio_buffers_get_channel(resampler->input_buffer, ch);
    double* out = audio_chunk_get_channel(output, ch);
    if (!buf || !out) continue;
    for (size_t frame = 0; frame < output_frames; frame++) {
      double idx = idx_buf[frame];
      double idx_floor = floor(idx);
      int start_idx = (int)idx_floor;
      int frac = (int)floor((idx - idx_floor) * factor_d);
      double frac_offset = frac_buf[frame];

      // get_nearest_times_3: sub = 0, 1, 2.
      adjust_point_t p0t = adjust_point(start_idx, frac, 0, factor);
      adjust_point_t p1t = adjust_point(start_idx, frac, 1, factor);
      adjust_point_t p2t = adjust_point(start_idx, frac, 2, factor);

      double p0 = sinc_dot_product(buf + p0t.idx + two_s_len,
                                   table + p0t.sub * s_len, s_len);
      double p1 = sinc_dot_product(buf + p1t.idx + two_s_len,
                                   table + p1t.sub * s_len, s_len);
      double p2 = sinc_dot_product(buf + p2t.idx + two_s_len,
                                   table + p2t.sub * s_len, s_len);

      // interp_quad (asynchro_sinc.rs:145-154).
      double a2 = p0 - 2.0 * p1 + p2;
      double a1 = -3.0 * p0 + 4.0 * p1 - p2;
      double a0 = 2.0 * p0;
      double x = frac_offset;
      double x2 = x * x;
      out[frame] = 0.5 * (a0 + a1 * x + a2 * x2);
    }
  }
}

static void run_linear(async_sinc_resampler_t* resampler, size_t output_frames,
                       audio_chunk_t* output) {
  size_t s_len = resampler->sinc_len;
  size_t two_s_len = 2 * s_len;
  int factor = (int)resampler->oversampling_factor;
  double factor_d = (double)factor;
  const double* table = resampler->sinc_table;
  const double* idx_buf = resampler->idx_scratch;
  const double* frac_buf = resampler->frac_scratch;

  for (size_t ch = 0; ch < resampler->channels; ch++) {
    const double* buf = audio_buffers_get_channel(resampler->input_buffer, ch);
    double* out = audio_chunk_get_channel(output, ch);
    if (!buf || !out) continue;
    for (size_t frame = 0; frame < output_frames; frame++) {
      double idx = idx_buf[frame];
      double idx_floor = floor(idx);
      int start_idx = (int)idx_floor;
      int frac = (int)floor((idx - idx_floor) * factor_d);
      double frac_offset = frac_buf[frame];

      // get_nearest_times_2: sub = 0, 1.
      adjust_point_t p0t = adjust_point(start_idx, frac, 0, factor);
      adjust_point_t p1t = adjust_point(start_idx, frac, 1, factor);

      double p0 = sinc_dot_product(buf + p0t.idx + two_s_len,
                                   table + p0t.sub * s_len, s_len);
      double p1 = sinc_dot_product(buf + p1t.idx + two_s_len,
                                   table + p1t.sub * s_len, s_len);

      // interp_lin: y0 + x * (y1 - y0).
      out[frame] = p0 + frac_offset * (p1 - p0);
    }
  }
}

resampler_error_t async_sinc_resampler_process(
    async_sinc_resampler_t* resampler, const audio_chunk_t* input,
    audio_chunk_t* output) {
  if (!resampler || !input || !output) return RESAMPLER_ERR_INVALID_PARAMETER;
  if (input->valid_frames != resampler->chunk_size) {
    return RESAMPLER_ERR_INPUT_SIZE_MISMATCH;
  }
  if (audio_chunk_get_channels(output) != resampler->channels) {
    return RESAMPLER_ERR_CHANNEL_COUNT_MISMATCH;
  }
  size_t output_frames = get_next_output_frames(resampler);
  if (audio_chunk_get_frames(output) < output_frames) {
    return RESAMPLER_ERR_OUTPUT_BUFFER_TOO_SMALL;
  }

  // Shift buffer, write new data, run inner.
  size_t s_len = resampler->sinc_len;
  size_t two_s_len = 2 * s_len;

  for (size_t ch = 0; ch < resampler->channels; ch++) {
    double* base = audio_buffers_get_channel(resampler->input_buffer, ch);
    if (!base) continue;
    // Copy [chunkSize..chunkSize + 2*sincLen] to [0..2*sincLen]
    // (shift remaining data to the beginning).
    memmove(base, base + resampler->chunk_size, two_s_len * sizeof(double));
  }
  for (size_t ch = 0; ch < resampler->channels; ch++) {
    const double* src_ptr = audio_chunk_get_channel(input, ch);
    double* dst_ptr = audio_buffers_get_channel(resampler->input_buffer, ch);
    if (!src_ptr || !dst_ptr) continue;
    memcpy(dst_ptr + two_s_len, src_ptr,
           resampler->chunk_size * sizeof(double));
  }

  // Pre-compute per-frame `idx` and `fracOffset`. Prologue of
  // the per-frame loop:
  //   t_ratio += t_ratio_increment;
  //   idx += t_ratio;
  //   frac = idx*factor - (idx*factor).floor();
  double t_ratio_start = 1.0 / resampler->resample_ratio;
  double t_ratio_end = 1.0 / resampler->target_ratio;
  double t_ratio_increment =
      (t_ratio_end - t_ratio_start) / (double)output_frames;
  double factor_d = (double)resampler->oversampling_factor;

  double t_ratio = t_ratio_start;
  double idx = resampler->last_index;
  for (size_t frame = 0; frame < output_frames; frame++) {
    t_ratio += t_ratio_increment;
    idx += t_ratio;
    resampler->idx_scratch[frame] = idx;
    double scaled = idx * factor_d;
    resampler->frac_scratch[frame] = scaled - floor(scaled);
  }
  double final_idx = idx;

  // Inner loop, specialised per interpolation mode.
  switch (resampler->interpolation) {
    case SINC_INTERPOLATION_NEAREST:
      run_nearest(resampler, output_frames, output);
      break;
    case SINC_INTERPOLATION_LINEAR:
      run_linear(resampler, output_frames, output);
      break;
    case SINC_INTERPOLATION_QUADRATIC:
      run_quadratic(resampler, output_frames, output);
      break;
    case SINC_INTERPOLATION_CUBIC:
      run_cubic(resampler, output_frames, output);
      break;
    default:
      run_cubic(resampler, output_frames, output);
      break;
  }

  // Update state for next chunk.
  resampler->last_index = final_idx - (double)resampler->chunk_size;
  resampler->resample_ratio = resampler->target_ratio;
  output->valid_frames = output_frames;
  return RESAMPLER_OK;
}
