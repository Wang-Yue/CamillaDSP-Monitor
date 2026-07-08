#if defined(__linux__)
#define _GNU_SOURCE
#endif
#define _DARWIN_C_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../Sources/CDSP/Audio/audio_chunk.h"
#include "../../Sources/CDSP/Resampler/audio_resampler.h"
#include "test_support.h"

typedef struct {
  int in_rate;
  int out_rate;
  const char* label;
} rate_pair_t;

static const rate_pair_t g_rate_grid[] = {
    {44100, 48000, "44.1→48k"},   {48000, 44100, "48→44.1k"},
    {48000, 96000, "48→96k"},     {96000, 48000, "96→48k"},
    {44100, 88200, "44.1→88.2k"}, {88200, 44100, "88.2→44.1k"},
    {44100, 192000, "44.1→192k"}, {192000, 44100, "192→44.1k"},
    {61900, 64000, "61.9→64k"}};

static const size_t g_rate_grid_count =
    sizeof(g_rate_grid) / sizeof(g_rate_grid[0]);

typedef struct {
  const char* label;
  bool has_aliasing_max;
  double aliasing_max_db;
  double passband_max_db;
  double symmetry_max;
  double sinad_min_db;
} swift_bounds_t;

static const swift_bounds_t g_swift_bounds[] = {
    {"44.1→48k", false, 0.0, 2.0e-2, 0.92, 208.0},
    {"48→44.1k", true, -185.0, 5.0e-3, 0.70, 204.0},
    {"48→96k", false, 0.0, 1.0e-2, 2.0e-15, 205.0},
    {"96→48k", true, -228.0, 2.5e-2, 2.0e-12, 205.0},
    {"44.1→88.2k", false, 0.0, 1.0e-2, 2.0e-15, 208.0},
    {"88.2→44.1k", true, -228.0, 2.5e-2, 2.0e-12, 204.0},
    {"44.1→192k", false, 0.0, 2.0e-2, 0.32, 200.0},
    {"192→44.1k", true, -230.0, 1.0e-2, 0.45, 199.0},
    {"37k→41k", false, 0.0, 6.2e-3, 0.21, 250.0}};

static const size_t g_swift_bounds_count =
    sizeof(g_swift_bounds) / sizeof(g_swift_bounds[0]);

typedef struct {
  bool has_aliasing_db;
  double aliasing_db;
  bool has_passband_db;
  double passband_db;
  bool has_symmetry;
  double symmetry;
  bool has_sinad_db;
  double sinad_db;
  bool has_ns_per_out_frame;
  double ns_per_out_frame;
  bool has_rtf_per_iter;
  double rtf_per_iter;
} cell_t;

typedef struct {
  int index;
  const char* label;
  cell_t swift_sync;
  cell_t swift_poly;
  cell_t swift_sinc;
  cell_t apple_mast;
  cell_t apple_minph;
  cell_t rubato_fft;
  cell_t rubato_poly;
  cell_t rubato_sinc;
} row_t;

typedef enum {
  METRIC_SINAD = 0,
  METRIC_ALIASING,
  METRIC_PASSBAND,
  METRIC_SYMMETRY,
  METRIC_NS_PER_FRAME,
  METRIC_RTF
} metric_type_t;

static char g_rubato_bin_path[1024] = {0};
static bool g_rubato_checked = false;
static bool g_rubato_available = false;

static bool check_rubato_available(void) {
  if (g_rubato_checked) return g_rubato_available;
  g_rubato_checked = true;

  const char* env_path = getenv("RUBATO_BIN");
  if (env_path && strlen(env_path) > 0) {
    FILE* f = fopen(env_path, "r");
    if (f) {
      fclose(f);
      strncpy(g_rubato_bin_path, env_path, sizeof(g_rubato_bin_path) - 1);
      g_rubato_available = true;
      return true;
    }
  }

  const char* candidates[] = {
      "Tests/RustHarnesses/target/release/cdsp_resampler_compare",
      "./Tests/RustHarnesses/target/release/cdsp_resampler_compare",
      "../Tests/RustHarnesses/target/release/cdsp_resampler_compare",
      "../../Tests/RustHarnesses/target/release/cdsp_resampler_compare",
      "/Users/wangyue/CamillaDSP-Monitor/Tests/RustHarnesses/target/release/"
      "cdsp_resampler_compare"};
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
    FILE* f = fopen(candidates[i], "r");
    if (f) {
      fclose(f);
      strncpy(g_rubato_bin_path, candidates[i], sizeof(g_rubato_bin_path) - 1);
      g_rubato_available = true;
      return true;
    }
  }
  printf(
      "⚠️ skipping: rubato harness not found — build with `make -C "
      "Tests/RustHarnesses`\n");
  g_rubato_available = false;
  return false;
}

static void pad_to_14(const char* str, char* out_buf) {
  size_t len = strlen(str);
  if (len >= 14) {
    memcpy(out_buf, str, 14);
    out_buf[14] = '\0';
  } else {
    memcpy(out_buf, str, len);
    for (size_t i = len; i < 14; i++) {
      out_buf[i] = ' ';
    }
    out_buf[14] = '\0';
  }
}

static double* make_sine(int rate, double freq, size_t samples) {
  double* out = (double*)calloc(samples, sizeof(double));
  if (!out) return NULL;
  double omega = 2.0 * M_PI * freq / (double)rate;
  for (size_t i = 0; i < samples; i++) {
    out[i] = sin(omega * (double)i);
  }
  return out;
}

static double compute_rms(const double* samples, size_t count) {
  if (count == 0) return 0.0;
  double sum_sq = 0.0;
  for (size_t i = 0; i < count; i++) {
    sum_sq += samples[i] * samples[i];
  }
  return sqrt(sum_sq / (double)count);
}

static double sinc_interp(const double* output, size_t count, double t) {
  int t_int = (int)floor(t);
  double frac = t - (double)t_int;
  if (fabs(frac) < 1e-9) {
    if (t_int >= 0 && (size_t)t_int < count) return output[t_int];
    return 0.0;
  }
  double val = 0.0;
  int kernel_radius = 12;
  for (int m = -kernel_radius; m <= kernel_radius; m++) {
    int n = t_int + m;
    if (n < 0 || (size_t)n >= count) continue;
    double x = (double)m - frac;
    double sinc_val = sin(M_PI * x) / (M_PI * x);
    double w = 0.5 * (1.0 + cos(M_PI * (double)m / (double)kernel_radius));
    val += output[n] * sinc_val * w;
  }
  return val;
}

static double impulse_asymmetry(const double* output, size_t count,
                                int window) {
  if (!output || count == 0) return NAN;
  size_t peak_idx = 0;
  double peak_val = 0.0;
  for (size_t i = 0; i < count; i++) {
    if (fabs(output[i]) > peak_val) {
      peak_idx = i;
      peak_val = fabs(output[i]);
    }
  }
  if (peak_idx + 16 >= count) return NAN;
  size_t rem = count - peak_idx - 16;
  size_t win_s = peak_idx < rem ? peak_idx : rem;
  if ((size_t)window < win_s) win_s = (size_t)window;
  int win = (int)win_s;

  if (win < 1 || peak_val == 0.0) return NAN;

  double min_asym = INFINITY;
  for (int step = -20; step <= 20; step++) {
    double delta = (double)step / 40.0;
    double center = (double)peak_idx + delta;
    double max_asym = 0.0;
    for (int k = 1; k <= win; k++) {
      double l = sinc_interp(output, count, center - (double)k);
      double r = sinc_interp(output, count, center + (double)k);
      double asym = fabs(l - r);
      if (asym > max_asym) max_asym = asym;
    }
    if (max_asym < min_asym) min_asym = max_asym;
  }
  return min_asym / peak_val;
}

static double sinad_db(const double* signal, size_t n, int rate, double freq) {
  if (n == 0) return 0.0;
  double omega = 2.0 * M_PI * freq / (double)rate;
  double sum_c1 = 0.0, sum_s1 = 0.0, sum_cc = 0.0, sum_ss = 0.0, sum_cs = 0.0;
  for (size_t t = 0; t < n; t++) {
    double c = cos(omega * (double)t);
    double s = sin(omega * (double)t);
    double y = signal[t];
    sum_c1 += y * c;
    sum_s1 += y * s;
    sum_cc += c * c;
    sum_ss += s * s;
    sum_cs += c * s;
  }
  double det = sum_cc * sum_ss - sum_cs * sum_cs;
  if (fabs(det) <= 1e-12) return 0.0;
  double I_val = (sum_ss * sum_c1 - sum_cs * sum_s1) / det;
  double Q_val = (sum_cc * sum_s1 - sum_cs * sum_c1) / det;
  double sum_sq_err = 0.0;
  for (size_t t = 0; t < n; t++) {
    double c = cos(omega * (double)t);
    double s = sin(omega * (double)t);
    double fitted = I_val * c + Q_val * s;
    double diff = signal[t] - fitted;
    sum_sq_err += diff * diff;
  }
  double signal_power = (I_val * I_val + Q_val * Q_val) / 2.0;
  double noise_power = sum_sq_err / (double)n;
  if (noise_power <= 0.0) return INFINITY;
  return 10.0 * log10(signal_power / noise_power);
}

static bool write_raw_f64(const double* data, size_t count, const char* path) {
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  size_t written = fwrite(data, sizeof(double), count, f);
  fclose(f);
  return written == count;
}

static double* read_raw_f64(const char* path, size_t* out_count) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0 || size % (long)sizeof(double) != 0) {
    fclose(f);
    return NULL;
  }
  size_t count = (size_t)size / sizeof(double);
  double* buf = (double*)malloc(count * sizeof(double));
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t read_cnt = fread(buf, sizeof(double), count, f);
  fclose(f);
  if (read_cnt != count) {
    free(buf);
    return NULL;
  }
  *out_count = count;
  return buf;
}

static double* run_resampler_full(audio_resampler_t* res, const double* input,
                                  size_t input_count, size_t* out_count) {
  size_t cs = audio_resampler_get_chunk_size(res);
  size_t max_out_per_chunk = audio_resampler_get_max_output_frames(res);
  double ratio = audio_resampler_get_ratio(res);

  size_t estimated_out = (size_t)((double)input_count * ratio) + 1024;
  double* output = (double*)calloc(estimated_out, sizeof(double));
  if (!output) return NULL;

  audio_chunk_t* in_chunk = audio_chunk_create(cs, 1);
  audio_chunk_t* out_chunk = audio_chunk_create(max_out_per_chunk, 1);

  size_t idx = 0;
  size_t total_out = 0;
  while (idx + cs <= input_count) {
    double* ch0 = audio_chunk_get_channel(in_chunk, 0);
    memcpy(ch0, input + idx, cs * sizeof(double));
    audio_chunk_set_valid_frames(in_chunk, cs);

    if (audio_resampler_process(res, in_chunk, out_chunk) == RESAMPLER_OK) {
      size_t produced = audio_chunk_get_valid_frames(out_chunk);
      if (total_out + produced > estimated_out) {
        estimated_out = (total_out + produced) * 2 + 1024;
        double* new_out =
            (double*)realloc(output, estimated_out * sizeof(double));
        if (!new_out) {
          free(output);
          audio_chunk_free(in_chunk);
          audio_chunk_free(out_chunk);
          return NULL;
        }
        output = new_out;
      }
      const double* out_ch0 = audio_chunk_get_channel(out_chunk, 0);
      memcpy(output + total_out, out_ch0, produced * sizeof(double));
      total_out += produced;
    }
    idx += cs;
  }

  audio_chunk_free(in_chunk);
  audio_chunk_free(out_chunk);
  *out_count = total_out;
  return output;
}

static double* run_rubato(const char* mode, int in_rate, int out_rate,
                          const double* input, size_t input_count,
                          size_t* out_count) {
  char in_path[256];
  char out_path[256];
  snprintf(in_path, sizeof(in_path), "/tmp/cdsp_matrix_%s_%d_%d_%zu_in.raw",
           mode, in_rate, out_rate, input_count);
  snprintf(out_path, sizeof(out_path), "/tmp/cdsp_matrix_%s_%d_%d_%zu_out.raw",
           mode, in_rate, out_rate, input_count);

  if (!write_raw_f64(input, input_count, in_path)) return NULL;

  char cmd[1024];
  snprintf(cmd, sizeof(cmd),
           "\"%s\" %s \"%s\" \"%s\" %d %d %d --no-partial >/dev/null 2>&1",
           g_rubato_bin_path, mode, in_path, out_path, in_rate, out_rate, 1024);
  int status = system(cmd);
  if (status != 0) return NULL;

  return read_raw_f64(out_path, out_count);
}

static double* run_process(int impl_id, const double* input, size_t input_count,
                           int in_rate, int out_rate, size_t* out_count) {
  resampler_config_t cfg;
  switch (impl_id) {
    case 0:
      resampler_config_init(&cfg, RESAMPLER_TYPE_SYNCHRONOUS);
      break;
    case 1:
      resampler_config_init(&cfg, RESAMPLER_TYPE_ASYNC_POLY);
      strncpy(cfg.interpolation, "Septic", sizeof(cfg.interpolation) - 1);
      cfg.has_interpolation = true;
      break;
    case 2:
      resampler_config_init(&cfg, RESAMPLER_TYPE_ASYNC_SINC);
      strncpy(cfg.profile, "Accurate", sizeof(cfg.profile) - 1);
      cfg.has_profile = true;
      break;
#if defined(ENABLE_COREAUDIO)
    case 3:
      resampler_config_init(&cfg, RESAMPLER_TYPE_APPLE);
      cfg.apple_quality = APPLE_RESAMPLER_QUALITY_MAX;
      cfg.has_apple_quality = true;
      cfg.apple_complexity = APPLE_RESAMPLER_COMPLEXITY_MASTERING;
      cfg.has_apple_complexity = true;
      break;
    case 4:
      resampler_config_init(&cfg, RESAMPLER_TYPE_APPLE);
      cfg.apple_quality = APPLE_RESAMPLER_QUALITY_MAX;
      cfg.has_apple_quality = true;
      cfg.apple_complexity = APPLE_RESAMPLER_COMPLEXITY_MINIMUM_PHASE;
      cfg.has_apple_complexity = true;
      break;
#endif
    case 5:
      if (!check_rubato_available()) return NULL;
      return run_rubato("fft", in_rate, out_rate, input, input_count,
                        out_count);
    case 6:
      if (!check_rubato_available()) return NULL;
      return run_rubato("poly-septic", in_rate, out_rate, input, input_count,
                        out_count);
    case 7:
      if (!check_rubato_available()) return NULL;
      return run_rubato("sinc-accurate", in_rate, out_rate, input, input_count,
                        out_count);
    default:
      return NULL;
  }

  size_t cs = 1024;
  audio_resampler_t* res =
      audio_resampler_create_from_config(&cfg, in_rate, out_rate, 1, cs);
  if (!res) return NULL;
  double* out = run_resampler_full(res, input, input_count, out_count);
  audio_resampler_free(res);
  return out;
}

static cell_t measure_quality_cell(int in_rate, int out_rate, int impl_id) {
  cell_t c;
  memset(&c, 0, sizeof(c));
  size_t cs = 1024;
  size_t nbr_in = 64 * cs;
  size_t out_skip_val = 4 * cs * out_rate / in_rate;
  size_t out_skip = out_skip_val > 1 ? out_skip_val : 1;

  if (out_rate < in_rate) {
    double out_ny = 0.5 * (double)out_rate;
    double in_ny = 0.5 * (double)in_rate;
    double test_freq = 0.5 * (out_ny + in_ny);
    double* signal = make_sine(in_rate, test_freq, nbr_in);
    size_t out_count = 0;
    double* out =
        run_process(impl_id, signal, nbr_in, in_rate, out_rate, &out_count);
    if (out && out_count > out_skip) {
      double rms_val = compute_rms(out + out_skip, out_count - out_skip);
      c.aliasing_db = 20.0 * log10(rms_val / sqrt(0.5));
      c.has_aliasing_db = true;
    }
    free(signal);
    if (out) free(out);
  }

  double min_ny = 0.5 * (double)(in_rate < out_rate ? in_rate : out_rate);
  double fractions[] = {0.001, 0.05, 0.5, 0.7, 0.85};
  double max_dev = -1.0;
  for (int i = 0; i < 5; i++) {
    double* signal = make_sine(in_rate, fractions[i] * min_ny, nbr_in);
    size_t out_count = 0;
    double* out =
        run_process(impl_id, signal, nbr_in, in_rate, out_rate, &out_count);
    if (out && out_count > out_skip) {
      double rms_val = compute_rms(out + out_skip, out_count - out_skip);
      double dev = fabs(20.0 * log10(rms_val / sqrt(0.5)));
      if (max_dev < 0.0 || dev > max_dev) {
        max_dev = dev;
      }
    }
    free(signal);
    if (out) free(out);
  }
  if (max_dev >= 0.0) {
    c.passband_db = max_dev;
    c.has_passband_db = true;
  }

  size_t imp_count = 32 * cs;
  double* imp_signal = (double*)calloc(imp_count, sizeof(double));
  if (imp_signal) {
    imp_signal[16 * cs] = 1.0;
    size_t imp_out_count = 0;
    double* imp_out = run_process(impl_id, imp_signal, imp_count, in_rate,
                                  out_rate, &imp_out_count);
    if (imp_out && imp_out_count > 0) {
      double asym = impulse_asymmetry(imp_out, imp_out_count, 256);
      if (!isnan(asym)) {
        c.symmetry = asym;
        c.has_symmetry = true;
      }
    }
    free(imp_signal);
    if (imp_out) free(imp_out);
  }

  double* sin_signal = make_sine(in_rate, 1000.0, nbr_in);
  if (sin_signal) {
    size_t up_count = 0;
    double* up_out =
        run_process(impl_id, sin_signal, nbr_in, in_rate, out_rate, &up_count);
    if (up_out && up_count > 0) {
      size_t down_count = 0;
      double* down_out = run_process(impl_id, up_out, up_count, out_rate,
                                     in_rate, &down_count);
      if (down_out && down_count > 0) {
        size_t skip = 4 * cs;
        size_t end_idx = skip + 8192 < down_count ? skip + 8192 : down_count;
        if (end_idx > skip) {
          double sinad =
              sinad_db(down_out + skip, end_idx - skip, in_rate, 1000.0);
          if (!isnan(sinad)) {
            c.sinad_db = sinad;
            c.has_sinad_db = true;
          }
        }
        free(down_out);
      }
      free(up_out);
    }
    free(sin_signal);
  }

  return c;
}

static bool measure_swift_perf(int in_rate, int out_rate, int impl_id,
                               double* out_ns_per_frame, double* out_rtf) {
  resampler_config_t cfg;
  switch (impl_id) {
    case 0:
      resampler_config_init(&cfg, RESAMPLER_TYPE_SYNCHRONOUS);
      break;
    case 1:
      resampler_config_init(&cfg, RESAMPLER_TYPE_ASYNC_POLY);
      strncpy(cfg.interpolation, "Septic", sizeof(cfg.interpolation) - 1);
      cfg.has_interpolation = true;
      break;
    case 2:
      resampler_config_init(&cfg, RESAMPLER_TYPE_ASYNC_SINC);
      strncpy(cfg.profile, "Accurate", sizeof(cfg.profile) - 1);
      cfg.has_profile = true;
      break;
#if defined(ENABLE_COREAUDIO)
    case 3:
      resampler_config_init(&cfg, RESAMPLER_TYPE_APPLE);
      cfg.apple_quality = APPLE_RESAMPLER_QUALITY_MAX;
      cfg.has_apple_quality = true;
      cfg.apple_complexity = APPLE_RESAMPLER_COMPLEXITY_MASTERING;
      cfg.has_apple_complexity = true;
      break;
    case 4:
      resampler_config_init(&cfg, RESAMPLER_TYPE_APPLE);
      cfg.apple_quality = APPLE_RESAMPLER_QUALITY_MAX;
      cfg.has_apple_quality = true;
      cfg.apple_complexity = APPLE_RESAMPLER_COMPLEXITY_MINIMUM_PHASE;
      cfg.has_apple_complexity = true;
      break;
#endif
    default:
      return false;
  }

  size_t cs = 1024;
  audio_resampler_t* resampler =
      audio_resampler_create_from_config(&cfg, in_rate, out_rate, 1, cs);
  if (!resampler) return false;
  cs = audio_resampler_get_chunk_size(resampler);

  size_t chunk_count = 64;
  size_t nbr_in = chunk_count * cs;

  audio_chunk_t* chunks[64];
  for (size_t c = 0; c < chunk_count; c++) {
    chunks[c] = audio_chunk_create(cs, 1);
    double* ch0 = audio_chunk_get_channel(chunks[c], 0);
    for (size_t i = 0; i < cs; i++) {
      ch0[i] = ((double)rand() / (double)RAND_MAX) * 2.0 - 1.0;
    }
    audio_chunk_set_valid_frames(chunks[c], cs);
  }

  size_t max_out = audio_resampler_get_max_output_frames(resampler);
  audio_chunk_t* scratch = audio_chunk_create(max_out, 1);

  for (size_t c = 0; c < chunk_count; c++) {
    audio_resampler_process(resampler, chunks[c], scratch);
  }

  struct timespec ts_start, ts_now;
  clock_gettime(CLOCK_MONOTONIC, &ts_start);
  double start_ns = (double)ts_start.tv_sec * 1e9 + (double)ts_start.tv_nsec;

  size_t out_frames = 0;
  int iters = 0;
  while (true) {
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    double now_ns = (double)ts_now.tv_sec * 1e9 + (double)ts_now.tv_nsec;
    if (iters >= 20 && (now_ns - start_ns) >= 400000000.0) {
      break;
    }
    for (size_t c = 0; c < chunk_count; c++) {
      if (audio_resampler_process(resampler, chunks[c], scratch) ==
          RESAMPLER_OK) {
        out_frames += audio_chunk_get_valid_frames(scratch);
      }
    }
    iters++;
  }

  clock_gettime(CLOCK_MONOTONIC, &ts_now);
  double end_ns = (double)ts_now.tv_sec * 1e9 + (double)ts_now.tv_nsec;
  double elapsed_ns = end_ns - start_ns;

  for (size_t c = 0; c < chunk_count; c++) {
    audio_chunk_free(chunks[c]);
  }
  audio_chunk_free(scratch);
  audio_resampler_free(resampler);

  if (out_frames == 0 || iters == 0) return false;

  *out_ns_per_frame = elapsed_ns / (double)out_frames;
  double in_sec = (double)nbr_in / (double)in_rate;
  *out_rtf = in_sec / (elapsed_ns * 1e-9 / (double)iters);
  return true;
}

static bool measure_rubato_perf(const char* mode, int in_rate, int out_rate,
                                double* out_ns_per_frame, double* out_rtf) {
  if (!check_rubato_available()) return false;

  size_t cs = 1024;
  size_t chunk_count = 64;
  size_t nbr_in = chunk_count * cs;

  double* input = (double*)malloc(nbr_in * sizeof(double));
  if (!input) return false;
  for (size_t i = 0; i < nbr_in; i++) {
    input[i] = ((double)rand() / (double)RAND_MAX) * 2.0 - 1.0;
  }

  char in_path[256];
  char out_path[256];
  snprintf(in_path, sizeof(in_path), "/tmp/cdsp_matrix_perf_%s_%d_%d_in.raw",
           mode, in_rate, out_rate);
  snprintf(out_path, sizeof(out_path), "/tmp/cdsp_matrix_perf_%s_%d_%d_out.raw",
           mode, in_rate, out_rate);

  if (!write_raw_f64(input, nbr_in, in_path)) {
    free(input);
    return false;
  }
  free(input);

  char cmd[1024];
  snprintf(cmd, sizeof(cmd),
           "\"%s\" %s \"%s\" \"%s\" %d %d %zu --bench=20 2>&1",
           g_rubato_bin_path, mode, in_path, out_path, in_rate, out_rate, cs);

  FILE* fp = popen(cmd, "r");
  if (!fp) return false;

  char line[512];
  unsigned long long ns_total = 0;
  int out_frames_per_iter = 0;
  int iters = 0;

  while (fgets(line, sizeof(line), fp)) {
    char* p = strstr(line, "BENCH_NS_TOTAL=");
    if (p) ns_total = strtoull(p + 15, NULL, 10);
    p = strstr(line, "BENCH_OUT_FRAMES_PER_ITER=");
    if (p) out_frames_per_iter = (int)strtol(p + 26, NULL, 10);
    p = strstr(line, "BENCH_ITERS=");
    if (p) iters = (int)strtol(p + 12, NULL, 10);
  }
  int status = pclose(fp);
  if (status != 0 || ns_total == 0 || out_frames_per_iter == 0 || iters == 0) {
    return false;
  }

  *out_ns_per_frame = (double)ns_total / (double)(out_frames_per_iter * iters);
  double in_sec = (double)nbr_in / (double)in_rate;
  *out_rtf = in_sec / ((double)ns_total * 1e-9 / (double)iters);
  return true;
}

static row_t compute_row_for_rate_pair(int index, int in_rate, int out_rate,
                                       const char* label, bool rubato_ok) {
  row_t r;
  memset(&r, 0, sizeof(r));
  r.index = index;
  r.label = label;

  r.swift_sync = measure_quality_cell(in_rate, out_rate, 0);
  r.swift_poly = measure_quality_cell(in_rate, out_rate, 1);
  r.swift_sinc = measure_quality_cell(in_rate, out_rate, 2);
  r.apple_mast = measure_quality_cell(in_rate, out_rate, 3);
  r.apple_minph = measure_quality_cell(in_rate, out_rate, 4);

  if (rubato_ok) {
    r.rubato_fft = measure_quality_cell(in_rate, out_rate, 5);
    r.rubato_poly = measure_quality_cell(in_rate, out_rate, 6);
    r.rubato_sinc = measure_quality_cell(in_rate, out_rate, 7);
  }

  double ns = 0.0, rtf = 0.0;
  if (measure_swift_perf(in_rate, out_rate, 0, &ns, &rtf)) {
    r.swift_sync.ns_per_out_frame = ns;
    r.swift_sync.has_ns_per_out_frame = true;
    r.swift_sync.rtf_per_iter = rtf;
    r.swift_sync.has_rtf_per_iter = true;
  }
  if (measure_swift_perf(in_rate, out_rate, 1, &ns, &rtf)) {
    r.swift_poly.ns_per_out_frame = ns;
    r.swift_poly.has_ns_per_out_frame = true;
    r.swift_poly.rtf_per_iter = rtf;
    r.swift_poly.has_rtf_per_iter = true;
  }
  if (measure_swift_perf(in_rate, out_rate, 2, &ns, &rtf)) {
    r.swift_sinc.ns_per_out_frame = ns;
    r.swift_sinc.has_ns_per_out_frame = true;
    r.swift_sinc.rtf_per_iter = rtf;
    r.swift_sinc.has_rtf_per_iter = true;
  }
  if (measure_swift_perf(in_rate, out_rate, 3, &ns, &rtf)) {
    r.apple_mast.ns_per_out_frame = ns;
    r.apple_mast.has_ns_per_out_frame = true;
    r.apple_mast.rtf_per_iter = rtf;
    r.apple_mast.has_rtf_per_iter = true;
  }
  if (measure_swift_perf(in_rate, out_rate, 4, &ns, &rtf)) {
    r.apple_minph.ns_per_out_frame = ns;
    r.apple_minph.has_ns_per_out_frame = true;
    r.apple_minph.rtf_per_iter = rtf;
    r.apple_minph.has_rtf_per_iter = true;
  }

  if (rubato_ok && r.rubato_fft.has_sinad_db) {
    if (measure_rubato_perf("fft", in_rate, out_rate, &ns, &rtf)) {
      r.rubato_fft.ns_per_out_frame = ns;
      r.rubato_fft.has_ns_per_out_frame = true;
      r.rubato_fft.rtf_per_iter = rtf;
      r.rubato_fft.has_rtf_per_iter = true;
    }
  }
  if (rubato_ok && r.rubato_poly.has_sinad_db) {
    if (measure_rubato_perf("poly-septic", in_rate, out_rate, &ns, &rtf)) {
      r.rubato_poly.ns_per_out_frame = ns;
      r.rubato_poly.has_ns_per_out_frame = true;
      r.rubato_poly.rtf_per_iter = rtf;
      r.rubato_poly.has_rtf_per_iter = true;
    }
  }
  if (rubato_ok && r.rubato_sinc.has_sinad_db) {
    if (measure_rubato_perf("sinc-accurate", in_rate, out_rate, &ns, &rtf)) {
      r.rubato_sinc.ns_per_out_frame = ns;
      r.rubato_sinc.has_ns_per_out_frame = true;
      r.rubato_sinc.rtf_per_iter = rtf;
      r.rubato_sinc.has_rtf_per_iter = true;
    }
  }

  return r;
}

static bool get_cell_metric(const cell_t* c, metric_type_t metric,
                            double* out_val) {
  switch (metric) {
    case METRIC_SINAD:
      if (c->has_sinad_db && isfinite(c->sinad_db)) {
        *out_val = c->sinad_db;
        return true;
      }
      return false;
    case METRIC_ALIASING:
      if (c->has_aliasing_db && isfinite(c->aliasing_db)) {
        *out_val = c->aliasing_db;
        return true;
      }
      return false;
    case METRIC_PASSBAND:
      if (c->has_passband_db && isfinite(c->passband_db)) {
        *out_val = c->passband_db;
        return true;
      }
      return false;
    case METRIC_SYMMETRY:
      if (c->has_symmetry && isfinite(c->symmetry)) {
        *out_val = c->symmetry;
        return true;
      }
      return false;
    case METRIC_NS_PER_FRAME:
      if (c->has_ns_per_out_frame && isfinite(c->ns_per_out_frame)) {
        *out_val = c->ns_per_out_frame;
        return true;
      }
      return false;
    case METRIC_RTF:
      if (c->has_rtf_per_iter && isfinite(c->rtf_per_iter)) {
        *out_val = c->rtf_per_iter;
        return true;
      }
      return false;
  }
  return false;
}

static void format_cell(bool has_val, double val, const char* format_str,
                        bool has_best, double best_val, char* out_14) {
  if (!has_val || !isfinite(val)) {
    pad_to_14(" N/A", out_14);
    return;
  }
  char raw_buf[64];
  snprintf(raw_buf, sizeof(raw_buf), format_str, val);

  char* start = raw_buf;
  while (*start == ' ' || *start == '\t') start++;
  char* end = start + strlen(start);
  while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\t')) {
    *(end - 1) = '\0';
    end--;
  }

  bool is_best = has_best && (val == best_val);
  char cell_str[64];
  if (is_best) {
    snprintf(cell_str, sizeof(cell_str), "(%s)", start);
  } else {
    snprintf(cell_str, sizeof(cell_str), " %s", start);
  }
  pad_to_14(cell_str, out_14);
}

static void print_table(const row_t* grid, size_t grid_count, const char* title,
                        metric_type_t metric, bool higher_is_better,
                        const char* format_str) {
  char pair_col[16], h0[16], h1[16], h2[16], h3[16], h4[16], h5[16], h6[16],
      h7[16];
  pad_to_14("Pair", pair_col);
  pad_to_14("C Sync", h0);
  pad_to_14("C Poly", h1);
  pad_to_14("C Sinc", h2);
  pad_to_14("Apple Mast", h3);
  pad_to_14("Apple MinPh", h4);
  pad_to_14("rubato Fft", h5);
  pad_to_14("rubato Poly", h6);
  pad_to_14("rubato Sinc", h7);

  const char* direction_str =
      higher_is_better ? "higher is better" : "lower is better";
  printf("=== %s (%s) ===\n", title, direction_str);
  printf("%s %s %s %s %s %s %s %s %s\n", pair_col, h0, h1, h2, h3, h4, h5, h6,
         h7);

  for (size_t r = 0; r < grid_count; r++) {
    const row_t* row = &grid[r];
    const cell_t* cells[8] = {&row->swift_sync,  &row->swift_poly,
                              &row->swift_sinc,  &row->apple_mast,
                              &row->apple_minph, &row->rubato_fft,
                              &row->rubato_poly, &row->rubato_sinc};
    double vals[8];
    bool has_vals[8];
    bool any_valid = false;
    double best_val = 0.0;
    bool has_best = false;

    for (int i = 0; i < 8; i++) {
      has_vals[i] = get_cell_metric(cells[i], metric, &vals[i]);
      if (has_vals[i]) {
        if (!any_valid) {
          any_valid = true;
          best_val = vals[i];
          has_best = true;
        } else {
          if (higher_is_better) {
            if (vals[i] > best_val) best_val = vals[i];
          } else {
            if (vals[i] < best_val) best_val = vals[i];
          }
        }
      }
    }

    if (!any_valid) continue;

    char c0[16], c1[16], c2[16], c3[16], c4[16], c5[16], c6[16], c7[16];
    format_cell(has_vals[0], vals[0], format_str, has_best, best_val, c0);
    format_cell(has_vals[1], vals[1], format_str, has_best, best_val, c1);
    format_cell(has_vals[2], vals[2], format_str, has_best, best_val, c2);
    format_cell(has_vals[3], vals[3], format_str, has_best, best_val, c3);
    format_cell(has_vals[4], vals[4], format_str, has_best, best_val, c4);
    format_cell(has_vals[5], vals[5], format_str, has_best, best_val, c5);
    format_cell(has_vals[6], vals[6], format_str, has_best, best_val, c6);
    format_cell(has_vals[7], vals[7], format_str, has_best, best_val, c7);

    char label_col[16];
    pad_to_14(row->label, label_col);
    printf("%s %s %s %s %s %s %s %s %s\n", label_col, c0, c1, c2, c3, c4, c5,
           c6, c7);
  }
}

TEST(compareAcrossRateGrid) {
  bool rubato_ok = check_rubato_available();

  row_t grid[9];
  for (size_t i = 0; i < g_rate_grid_count; i++) {
    grid[i] = compute_row_for_rate_pair((int)i, g_rate_grid[i].in_rate,
                                        g_rate_grid[i].out_rate,
                                        g_rate_grid[i].label, rubato_ok);
  }

  print_table(grid, g_rate_grid_count, "Round-trip SINAD (1 kHz sine)",
              METRIC_SINAD, true, "%6.1f dB");
  print_table(grid, g_rate_grid_count, "Aliasing rejection (mid-stopband tone)",
              METRIC_ALIASING, false, "%7.1f dB");
  print_table(grid, g_rate_grid_count, "Passband peak deviation",
              METRIC_PASSBAND, false, "%7.4f dB");
  print_table(grid, g_rate_grid_count,
              "Impulse-response asymmetry (rel to peak)", METRIC_SYMMETRY,
              false, "%9.3e");
  print_table(grid, g_rate_grid_count, "Throughput (ns/output-frame)",
              METRIC_NS_PER_FRAME, false, "%8.1f");
  print_table(grid, g_rate_grid_count,
              "Throughput (real-time factor per iteration)", METRIC_RTF, true,
              "%7.1fx");

  for (size_t i = 0; i < g_rate_grid_count; i++) {
    const row_t* entry = &grid[i];
    const swift_bounds_t* bounds = NULL;
    for (size_t b = 0; b < g_swift_bounds_count; b++) {
      if (strcmp(entry->label, g_swift_bounds[b].label) == 0) {
        bounds = &g_swift_bounds[b];
        break;
      }
    }
    if (!bounds) continue;

    const cell_t* c = &entry->swift_sync;
    if (bounds->has_aliasing_max && c->has_aliasing_db) {
      if (!(c->aliasing_db < bounds->aliasing_max_db)) {
        printf("[%s] C Sync aliasing %g dB shallower than target %g dB\n",
               entry->label, c->aliasing_db, bounds->aliasing_max_db);
      }
      ASSERT_TRUE(c->aliasing_db < bounds->aliasing_max_db);
    }
    if (c->has_passband_db) {
      if (!(c->passband_db < bounds->passband_max_db)) {
        printf("[%s] C Sync passband %g dB > target %g dB\n", entry->label,
               c->passband_db, bounds->passband_max_db);
      }
      ASSERT_TRUE(c->passband_db < bounds->passband_max_db);
    }
    if (c->has_symmetry) {
      if (!(c->symmetry < bounds->symmetry_max)) {
        printf("[%s] C Sync impulse asymmetry %g > target %g\n", entry->label,
               c->symmetry, bounds->symmetry_max);
      }
      ASSERT_TRUE(c->symmetry < bounds->symmetry_max);
    }
    if (c->has_sinad_db) {
      if (!(c->sinad_db > bounds->sinad_min_db)) {
        printf("[%s] C Sync round-trip SINAD %g dB < target %g dB\n",
               entry->label, c->sinad_db, bounds->sinad_min_db);
      }
      ASSERT_TRUE(c->sinad_db > bounds->sinad_min_db);
    }
  }
}

TEST_MAIN()
