#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../Sources/CDSP/Audio/audio_buffers.h"
#include "../../Sources/CDSP/Audio/audio_chunk.h"
#include "../../Sources/CDSP/Audio/processing_parameters.h"
#include "../../Sources/CDSP/DoP/dop_decoder.h"
#include "../../Sources/CDSP/DoP/dop_encoder.h"
#include "../../Sources/CDSP/Filters/biquad.h"
#include "../../Sources/CDSP/Filters/biquad_combo.h"
#include "../../Sources/CDSP/Filters/convolution.h"
#include "../../Sources/CDSP/Filters/delay.h"
#include "../../Sources/CDSP/Filters/diffeq.h"
#include "../../Sources/CDSP/Filters/dither.h"
#include "../../Sources/CDSP/Filters/gain.h"
#include "../../Sources/CDSP/Filters/limiter.h"
#include "../../Sources/CDSP/Filters/lookahead_limiter.h"
#include "../../Sources/CDSP/Filters/loudness.h"
#include "../../Sources/CDSP/Filters/volume.h"
#include "../../Sources/CDSP/Logging/app_logger.h"
#include "../../Sources/CDSP/Mixer/mixer.h"
#include "../../Sources/CDSP/Processors/compressor_processor.h"
#include "../../Sources/CDSP/Processors/noise_gate_processor.h"
#include "../../Sources/CDSP/Processors/race_processor.h"
#include "../../Sources/CDSP/Resampler/apple_resampler.h"
#include "../../Sources/CDSP/Resampler/async_poly_resampler.h"
#include "../../Sources/CDSP/Resampler/async_sinc_resampler.h"
#include "../../Sources/CDSP/Resampler/audio_resampler.h"
#include "../../Sources/CDSP/Resampler/synchronous_resampler.h"
#include "test_support.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef void (*malloc_logger_t)(uint32_t type, uintptr_t arg1, uintptr_t arg2,
                                uintptr_t arg3, uintptr_t result,
                                uint32_t num_hot_frames_to_skip);

static _Atomic uint64_t g_alloc_counter = 0;
static _Atomic uintptr_t g_watched_thread = 0;
static malloc_logger_t g_prev_logger = NULL;

static void my_malloc_logger(uint32_t type, uintptr_t arg1, uintptr_t arg2,
                             uintptr_t arg3, uintptr_t result,
                             uint32_t num_hot_frames_to_skip) {
  (void)arg1;
  (void)arg2;
  (void)arg3;
  (void)num_hot_frames_to_skip;
  if ((type & 2) != 0 && result != 0) {
    uintptr_t watched =
        atomic_load_explicit(&g_watched_thread, memory_order_acquire);
    if (watched != 0 && (uintptr_t)pthread_self() == watched) {
      atomic_fetch_add_explicit(&g_alloc_counter, 1, memory_order_relaxed);
    }
  }
  if (g_prev_logger) {
    g_prev_logger(type, arg1, arg2, arg3, result, num_hot_frames_to_skip);
  }
}

typedef void (*test_iter_func_t)(int iter, void* ctx);

typedef struct {
  test_iter_func_t body;
  int warmup;
  int iterations;
  void* ctx;
} loop_ctx_t;

static void run_test_loop(void* arg) {
  loop_ctx_t* l = (loop_ctx_t*)arg;
  for (int i = 0; i < l->iterations; i++) {
    l->body(l->warmup + i, l->ctx);
  }
}

static bool count_allocations(void (*body)(void*), void* ctx,
                              uint64_t* out_count) {
  void* handle = dlopen(NULL, RTLD_LAZY);
  if (!handle) return false;
  malloc_logger_t* logger_ptr =
      (malloc_logger_t*)dlsym(handle, "malloc_logger");
  if (!logger_ptr) {
    dlclose(handle);
    return false;
  }

  uintptr_t my_thread = (uintptr_t)pthread_self();
  g_prev_logger = *logger_ptr;
  atomic_store_explicit(&g_alloc_counter, 0, memory_order_relaxed);
  atomic_store_explicit(&g_watched_thread, my_thread, memory_order_release);
  *logger_ptr = my_malloc_logger;

  body(ctx);

  *logger_ptr = g_prev_logger;
  atomic_store_explicit(&g_watched_thread, 0, memory_order_release);
  *out_count = atomic_load_explicit(&g_alloc_counter, memory_order_relaxed);
  dlclose(handle);
  return true;
}

static void assert_allocation_free(const char* label, int warmup,
                                   int iterations, test_iter_func_t body,
                                   void* ctx) {
  for (int i = 0; i < warmup; i++) {
    body(i, ctx);
  }
  loop_ctx_t lctx = {body, warmup, iterations, ctx};
  uint64_t count = 0;
  if (!count_allocations(run_test_loop, &lctx, &count)) {
    printf("malloc_logger unavailable — %s skipped\n", label);
    return;
  }
  printf("[%s] allocations=%llu over %d iterations\n", label,
         (unsigned long long)count, iterations);
  ASSERT_TRUE(count < 10);
}

static audio_chunk_t** make_random_chunks(int count, int channels, int frames,
                                          double scale) {
  audio_chunk_t** chunks =
      (audio_chunk_t**)calloc(count, sizeof(audio_chunk_t*));
  for (int i = 0; i < count; i++) {
    chunks[i] = audio_chunk_create(frames, channels);
    for (int ch = 0; ch < channels; ch++) {
      double* wv = audio_chunk_get_channel(chunks[i], ch);
      for (int f = 0; f < frames; f++) {
        wv[f] = ((double)rand() / RAND_MAX) * 2.0 * scale - scale;
      }
    }
    audio_chunk_set_valid_frames(chunks[i], frames);
  }
  return chunks;
}

static void free_chunks(audio_chunk_t** chunks, int count) {
  if (!chunks) return;
  for (int i = 0; i < count; i++) {
    if (chunks[i]) audio_chunk_free(chunks[i]);
  }
  free(chunks);
}

static void fill_sine(mutable_waveform_t buf, int frames, double freq_hz,
                      double sample_rate) {
  for (int i = 0; i < frames; i++) {
    buf[i] = sin(2.0 * M_PI * freq_hz * (double)i / sample_rate);
  }
}

// MARK: - Resamplers

typedef struct {
  audio_resampler_t* resampler;
  audio_chunk_t** inputs;
  int input_count;
  audio_chunk_t* output;
} resampler_test_ctx_t;

static void resampler_iter(int i, void* ctx) {
  resampler_test_ctx_t* c = (resampler_test_ctx_t*)ctx;
  audio_resampler_process(c->resampler, c->inputs[i % c->input_count],
                          c->output);
}

static void run_resampler_hot_path(audio_resampler_t* resampler, int channels,
                                   const char* label) {
  int cs = (int)audio_resampler_get_chunk_size(resampler);
  int max_out = (int)audio_resampler_get_max_output_frames(resampler);
  audio_chunk_t** inputs = make_random_chunks(32, channels, cs, 1.0);
  audio_chunk_t* output = audio_chunk_create(max_out, channels);
  resampler_test_ctx_t ctx = {resampler, inputs, 32, output};
  assert_allocation_free(label, 0, 30, resampler_iter, &ctx);
  free_chunks(inputs, 32);
  audio_chunk_free(output);
}

#if defined(ENABLE_COREAUDIO)
TEST(AppleResampler_AllocationFree_Stereo) {
  resampler_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = RESAMPLER_TYPE_APPLE;
  cfg.apple_quality = APPLE_RESAMPLER_QUALITY_MAX;
  cfg.has_apple_quality = true;
  cfg.apple_complexity = APPLE_RESAMPLER_COMPLEXITY_NORMAL;
  cfg.has_apple_complexity = true;

  audio_resampler_t* res =
      audio_resampler_create_from_config(&cfg, 44100, 48000, 2, 1024);
  ASSERT_TRUE(res != NULL);
  run_resampler_hot_path(res, 2, "AppleResampler stereo");
  audio_resampler_free(res);
}
#endif  // ENABLE_COREAUDIO

TEST(Synchronous_Stereo) {
  resampler_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = RESAMPLER_TYPE_SYNCHRONOUS;

  audio_resampler_t* res =
      audio_resampler_create_from_config(&cfg, 44100, 48000, 2, 1024);
  ASSERT_TRUE(res != NULL);
  run_resampler_hot_path(res, 2, "Synchronous stereo");
  audio_resampler_free(res);
}

TEST(AsyncPoly_Stereo) {
  resampler_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = RESAMPLER_TYPE_ASYNC_POLY;
  strcpy(cfg.interpolation, "cubic");
  cfg.has_interpolation = true;

  audio_resampler_t* res =
      audio_resampler_create_from_config(&cfg, 44100, 48000, 2, 1024);
  ASSERT_TRUE(res != NULL);
  run_resampler_hot_path(res, 2, "AsyncPoly stereo");
  audio_resampler_free(res);
}

TEST(AsyncSinc_Stereo) {
  resampler_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = RESAMPLER_TYPE_ASYNC_SINC;
  strcpy(cfg.profile, "accurate");
  cfg.has_profile = true;

  audio_resampler_t* res =
      audio_resampler_create_from_config(&cfg, 44100, 48000, 2, 1024);
  ASSERT_TRUE(res != NULL);
  run_resampler_hot_path(res, 2, "AsyncSinc stereo");
  audio_resampler_free(res);
}

// MARK: - Filters

typedef struct {
  void* filter;
  void (*process)(void*, double*, size_t);
  double* wave;
  size_t frames;
} filter_test_ctx_t;

static void bq_process_wrap(void* f, double* w, size_t n) {
  biquad_filter_process((biquad_filter_t*)f, w, n);
}
static void conv_process_wrap(void* f, double* w, size_t n) {
  convolution_filter_process((convolution_filter_t*)f, w, n);
}
static void gain_process_wrap(void* f, double* w, size_t n) {
  gain_filter_process((gain_filter_t*)f, w, n);
}
static void loud_process_wrap(void* f, double* w, size_t n) {
  loudness_filter_process((loudness_filter_t*)f, w, n);
}
static void delay_process_wrap(void* f, double* w, size_t n) {
  delay_filter_process((delay_filter_t*)f, w, n);
}
static void combo_process_wrap(void* f, double* w, size_t n) {
  biquad_combo_filter_process((biquad_combo_filter_t*)f, w, n);
}
static void diffeq_process_wrap(void* f, double* w, size_t n) {
  diffeq_filter_process((diffeq_filter_t*)f, w, n);
}
static void dither_process_wrap(void* f, double* w, size_t n) {
  dither_filter_process((dither_filter_t*)f, w, n);
}
static void limit_process_wrap(void* f, double* w, size_t n) {
  limiter_filter_process((limiter_filter_t*)f, w, n);
}
static void look_process_wrap(void* f, double* w, size_t n) {
  lookahead_limiter_filter_process((lookahead_limiter_filter_t*)f, w, n);
}

static void filter_iter(int i, void* ctx) {
  (void)i;
  filter_test_ctx_t* c = (filter_test_ctx_t*)ctx;
  c->process(c->filter, c->wave, c->frames);
}

TEST(Biquad_AllocationFree) {
  biquad_parameters_t params = {
      .type = BIQUAD_TYPE_LOWPASS, .freq = 1000.0, .q = 0.707};
  biquad_coefficients_t coeffs = {
      .b0 = 0.25, .b1 = 0.5, .b2 = 0.25, .a1 = -0.5, .a2 = 0.1};
  (void)params;
  biquad_filter_t* filter = biquad_filter_create("bq", &coeffs);
  ASSERT_TRUE(filter != NULL);
  double* wave = (double*)calloc(1024, sizeof(double));
  fill_sine(wave, 1024, 1000.0, 44100.0);
  filter_test_ctx_t ctx = {filter, bq_process_wrap, wave, 1024};
  assert_allocation_free("Biquad", 0, 30, filter_iter, &ctx);
  free(wave);
  biquad_filter_free(filter);
}

TEST(Convolution_AllocationFree) {
  int chunk_size = 1024;
  int ir_len = 4096;
  double* ir = (double*)calloc(ir_len, sizeof(double));
  for (int i = 0; i < ir_len; i++) {
    ir[i] = (i == 0 ? 1.0 : 0.0) + 0.001 * cos((double)i * 0.01);
  }
  conv_parameters_t params = {
      .type = CONV_TYPE_VALUES, .values = ir, .values_count = ir_len};
  convolution_filter_t* filter =
      convolution_filter_create("conv", &params, chunk_size);
  ASSERT_TRUE(filter != NULL);
  double* wave = (double*)calloc(chunk_size, sizeof(double));
  fill_sine(wave, chunk_size, 1000.0, 44100.0);
  filter_test_ctx_t ctx = {filter, conv_process_wrap, wave, chunk_size};
  assert_allocation_free("Convolution", 3, 30, filter_iter, &ctx);
  free(wave);
  free(ir);
  convolution_filter_free(filter);
}

TEST(Gain_AllocationFree) {
  gain_parameters_t params = {
      .gain = -6.0, .has_gain = true, .scale = GAIN_SCALE_DB};
  gain_filter_t* filter = gain_filter_create("gain", &params);
  ASSERT_TRUE(filter != NULL);
  double* wave = (double*)calloc(1024, sizeof(double));
  fill_sine(wave, 1024, 1000.0, 44100.0);
  filter_test_ctx_t ctx = {filter, gain_process_wrap, wave, 1024};
  assert_allocation_free("Gain", 0, 30, filter_iter, &ctx);
  free(wave);
  gain_filter_free(filter);
}

static void vol_iter(int i, void* ctx) {
  (void)i;
  filter_test_ctx_t* c = (filter_test_ctx_t*)ctx;
  volume_filter_t* vf = (volume_filter_t*)c->filter;
  volume_filter_prepare_chunk(vf);
  volume_filter_process(vf, c->wave, c->frames);
  volume_filter_advance_ramp(vf);
}

TEST(Volume_AllocationFree) {
  processing_parameters_t* proc_params = processing_parameters_create(2, 2);
  processing_parameters_set_target_volume_for_fader(proc_params, -6.0,
                                                    FADER_MAIN);
  processing_parameters_set_muted_for_fader(proc_params, false, FADER_MAIN);
  volume_parameters_t params = {.ramp_time = 0.0,
                                .has_ramp_time = true,
                                .limit = 50.0,
                                .has_limit = true,
                                .fader = FADER_MAIN};
  volume_filter_t* filter =
      volume_filter_create("vol", &params, 44100, 1024, proc_params);
  ASSERT_TRUE(filter != NULL);
  double* wave = (double*)calloc(1024, sizeof(double));
  fill_sine(wave, 1024, 1000.0, 44100.0);
  filter_test_ctx_t ctx = {filter, NULL, wave, 1024};
  assert_allocation_free("Volume", 0, 30, vol_iter, &ctx);
  free(wave);
  volume_filter_free(filter);
  processing_parameters_free(proc_params);
}

TEST(Loudness_AllocationFree) {
  processing_parameters_t* proc_params = processing_parameters_create(2, 2);
  processing_parameters_set_current_volume_for_fader(proc_params, -45.0,
                                                     FADER_MAIN);
  loudness_parameters_t params = {.reference_level = -25.0,
                                  .has_reference_level = true,
                                  .high_boost = 10.0,
                                  .has_high_boost = true,
                                  .low_boost = 10.0,
                                  .has_low_boost = true,
                                  .attenuate_mid = false};
  loudness_filter_t* filter =
      loudness_filter_create("loud", &params, 44100, proc_params);
  ASSERT_TRUE(filter != NULL);
  double* wave = (double*)calloc(1024, sizeof(double));
  fill_sine(wave, 1024, 1000.0, 44100.0);
  filter_test_ctx_t ctx = {filter, loud_process_wrap, wave, 1024};
  assert_allocation_free("Loudness", 0, 30, filter_iter, &ctx);
  free(wave);
  loudness_filter_free(filter);
  processing_parameters_free(proc_params);
}

TEST(Delay_AllocationFree) {
  delay_parameters_t params = {
      .delay = 5.5, .unit = DELAY_UNIT_SAMPLES, .subsample = true};
  delay_filter_t* filter = delay_filter_create("del", &params, 44100);
  ASSERT_TRUE(filter != NULL);
  double* wave = (double*)calloc(1024, sizeof(double));
  fill_sine(wave, 1024, 1000.0, 44100.0);
  filter_test_ctx_t ctx = {filter, delay_process_wrap, wave, 1024};
  assert_allocation_free("Delay", 0, 30, filter_iter, &ctx);
  free(wave);
  delay_filter_free(filter);
}

TEST(BiquadCombo_AllocationFree) {
  biquad_combo_parameters_t params = {.type = BIQUAD_COMBO_TYPE_FIVE_POINT_PEQ,
                                      .fls = 80.0,
                                      .qls = 0.707,
                                      .gls = 3.0,
                                      .fp1 = 250.0,
                                      .qp1 = 1.5,
                                      .gp1 = -2.0,
                                      .fp2 = 1000.0,
                                      .qp2 = 2.0,
                                      .gp2 = 1.5,
                                      .fp3 = 4000.0,
                                      .qp3 = 1.0,
                                      .gp3 = -1.0,
                                      .fhs = 12000.0,
                                      .qhs = 0.707,
                                      .ghs = 2.5};
  biquad_combo_filter_t* filter =
      biquad_combo_filter_create("combo", &params, 44100);
  ASSERT_TRUE(filter != NULL);
  double* wave = (double*)calloc(1024, sizeof(double));
  fill_sine(wave, 1024, 1000.0, 44100.0);
  filter_test_ctx_t ctx = {filter, combo_process_wrap, wave, 1024};
  assert_allocation_free("BiquadCombo", 0, 30, filter_iter, &ctx);
  free(wave);
  biquad_combo_filter_free(filter);
}

TEST(DiffEq_AllocationFree) {
  double a[] = {1.0, -1.864844640491105, 0.8818236057002321};
  double b[] = {0.004244741301241303, 0.008489482602482605,
                0.004244741301241303};
  diff_eq_parameters_t params = {.a = a, .a_count = 3, .b = b, .b_count = 3};
  diffeq_filter_t* filter = diffeq_filter_create("diffeq", &params);
  ASSERT_TRUE(filter != NULL);
  double* wave = (double*)calloc(1024, sizeof(double));
  fill_sine(wave, 1024, 1000.0, 44100.0);
  filter_test_ctx_t ctx = {filter, diffeq_process_wrap, wave, 1024};
  assert_allocation_free("DiffEq", 0, 30, filter_iter, &ctx);
  free(wave);
  diffeq_filter_free(filter);
}

TEST(Dither_AllocationFree) {
  dither_parameters_t params = {.type = DITHER_TYPE_GESEMANN_441, .bits = 16};
  dither_filter_t* filter = dither_filter_create("dither", &params);
  ASSERT_TRUE(filter != NULL);
  double* wave = (double*)calloc(1024, sizeof(double));
  fill_sine(wave, 1024, 1000.0, 44100.0);
  filter_test_ctx_t ctx = {filter, dither_process_wrap, wave, 1024};
  assert_allocation_free("Dither", 0, 30, filter_iter, &ctx);
  free(wave);
  dither_filter_free(filter);
}

TEST(Limiter_AllocationFree) {
  limiter_parameters_t params = {.clip_limit = -1.5, .soft_clip = true};
  limiter_filter_t* filter = limiter_filter_create("limiter", &params);
  ASSERT_TRUE(filter != NULL);
  double* wave = (double*)calloc(1024, sizeof(double));
  fill_sine(wave, 1024, 1000.0, 44100.0);
  filter_test_ctx_t ctx = {filter, limit_process_wrap, wave, 1024};
  assert_allocation_free("Limiter", 0, 30, filter_iter, &ctx);
  free(wave);
  limiter_filter_free(filter);
}

TEST(LookaheadLimiter_AllocationFree) {
  lookahead_limiter_parameters_t params = {.limit = -1.0,
                                           .attack = 4.0,
                                           .release = 20.0,
                                           .unit = DELAY_UNIT_SAMPLES};
  lookahead_limiter_filter_t* filter =
      lookahead_limiter_filter_create("lookahead", &params, 44100, 1024);
  ASSERT_TRUE(filter != NULL);
  double* wave = (double*)calloc(1024, sizeof(double));
  fill_sine(wave, 1024, 1000.0, 44100.0);
  filter_test_ctx_t ctx = {filter, look_process_wrap, wave, 1024};
  assert_allocation_free("LookaheadLimiter", 0, 30, filter_iter, &ctx);
  free(wave);
  lookahead_limiter_filter_free(filter);
}

// MARK: - Processors

typedef struct {
  void* proc;
  void (*process)(void*, audio_chunk_t*);
  audio_chunk_t* chunk;
} proc_test_ctx_t;

static void comp_proc_wrap(void* p, audio_chunk_t* c) {
  compressor_processor_process((compressor_processor_t*)p, c);
}
static void gate_proc_wrap(void* p, audio_chunk_t* c) {
  noise_gate_processor_process((noise_gate_processor_t*)p, c);
}
static void race_proc_wrap(void* p, audio_chunk_t* c) {
  race_processor_process((race_processor_t*)p, c);
}

static void proc_iter(int i, void* ctx) {
  (void)i;
  proc_test_ctx_t* c = (proc_test_ctx_t*)ctx;
  c->process(c->proc, c->chunk);
}

TEST(Compressor_AllocationFree) {
  int mon_ch[] = {0};
  int proc_ch[] = {0, 1};
  compressor_parameters_t params = {.channels = 2,
                                    .monitor_channels = mon_ch,
                                    .monitor_channels_count = 1,
                                    .process_channels = proc_ch,
                                    .process_channels_count = 2,
                                    .attack = 0.005,
                                    .release = 0.05,
                                    .threshold = -10.0,
                                    .factor = 3.0,
                                    .makeup_gain = 2.0,
                                    .has_makeup_gain = true,
                                    .soft_clip = true,
                                    .clip_limit = -1.0,
                                    .has_clip_limit = true};
  compressor_processor_t* proc =
      compressor_processor_create("comp", &params, 44100, 1024);
  ASSERT_TRUE(proc != NULL);
  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  for (size_t f = 0; f < 1024; f++) {
    audio_chunk_get_channel(chunk, 0)[f] = 0.5;
    audio_chunk_get_channel(chunk, 1)[f] = 0.5;
  }
  audio_chunk_set_valid_frames(chunk, 1024);
  proc_test_ctx_t ctx = {proc, comp_proc_wrap, chunk};
  assert_allocation_free("Compressor", 0, 30, proc_iter, &ctx);
  audio_chunk_free(chunk);
  compressor_processor_free(proc);
}

TEST(NoiseGate_AllocationFree) {
  int mon_ch[] = {0};
  int proc_ch[] = {0, 1};
  noise_gate_parameters_t params = {.channels = 2,
                                    .monitor_channels = mon_ch,
                                    .monitor_channels_count = 1,
                                    .process_channels = proc_ch,
                                    .process_channels_count = 2,
                                    .attack = 0.005,
                                    .release = 0.05,
                                    .threshold = -20.0,
                                    .attenuation = 12.0};
  noise_gate_processor_t* proc =
      noise_gate_processor_create("gate", &params, 44100, 1024);
  ASSERT_TRUE(proc != NULL);
  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  for (size_t f = 0; f < 1024; f++) {
    audio_chunk_get_channel(chunk, 0)[f] = 0.5;
    audio_chunk_get_channel(chunk, 1)[f] = 0.5;
  }
  audio_chunk_set_valid_frames(chunk, 1024);
  proc_test_ctx_t ctx = {proc, gate_proc_wrap, chunk};
  assert_allocation_free("NoiseGate", 0, 30, proc_iter, &ctx);
  audio_chunk_free(chunk);
  noise_gate_processor_free(proc);
}

TEST(RACE_AllocationFree) {
  race_parameters_t params = {.channels = 2,
                              .channel_a = 0,
                              .channel_b = 1,
                              .delay = 12.0,
                              .subsample_delay = false,
                              .has_subsample_delay = true,
                              .delay_unit = DELAY_UNIT_SAMPLES,
                              .has_delay_unit = true,
                              .attenuation = 6.0};
  race_processor_t* proc = race_processor_create("race", &params, 44100);
  ASSERT_TRUE(proc != NULL);
  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  for (size_t f = 0; f < 1024; f++) {
    audio_chunk_get_channel(chunk, 0)[f] = 0.5;
    audio_chunk_get_channel(chunk, 1)[f] = 0.5;
  }
  audio_chunk_set_valid_frames(chunk, 1024);
  proc_test_ctx_t ctx = {proc, race_proc_wrap, chunk};
  assert_allocation_free("RACE", 0, 30, proc_iter, &ctx);
  audio_chunk_free(chunk);
  race_processor_free(proc);
}

// MARK: - Mixer

typedef struct {
  audio_mixer_t* mixer;
  audio_chunk_t** inputs;
  int input_count;
  audio_chunk_t* output;
} mixer_test_ctx_t;

static void mixer_iter(int i, void* ctx) {
  mixer_test_ctx_t* c = (mixer_test_ctx_t*)ctx;
  audio_mixer_process(c->mixer, c->inputs[i % c->input_count], c->output);
}

TEST(Mixer_2to4_AllocationFree) {
  mixer_source_t s00 = {
      .channel = 0, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB};
  mixer_source_t s11 = {
      .channel = 1, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB};
  mixer_source_t s20 = {
      .channel = 0, .gain = -3.0, .has_gain = true, .scale = GAIN_SCALE_DB};
  mixer_source_t s21 = {
      .channel = 1, .gain = -3.0, .has_gain = true, .scale = GAIN_SCALE_DB};
  mixer_source_t s2_srcs[] = {s20, s21};
  mixer_source_t s31 = {
      .channel = 1, .gain = -6.0, .has_gain = true, .scale = GAIN_SCALE_DB};
  mixer_mapping_t maps[4] = {
      {.dest = 0, .sources_count = 1, .sources = &s00},
      {.dest = 1, .sources_count = 1, .sources = &s11},
      {.dest = 2, .sources_count = 2, .sources = s2_srcs},
      {.dest = 3, .sources_count = 1, .sources = &s31}};
  mixer_config_t config = {
      .channels_in = 2, .channels_out = 4, .mapping_count = 4, .mapping = maps};
  audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 1024);
  ASSERT_TRUE(mixer != NULL);
  audio_chunk_t** inputs = make_random_chunks(32, 2, 1024, 1.0);
  audio_chunk_t* output = audio_chunk_create(1024, 4);
  mixer_test_ctx_t ctx = {mixer, inputs, 32, output};
  assert_allocation_free("Mixer 2->4", 0, 30, mixer_iter, &ctx);
  free_chunks(inputs, 32);
  audio_chunk_free(output);
  audio_mixer_free(mixer);
}

// MARK: - DoP

typedef struct {
  dop_encoder_t* encoder;
  audio_chunk_t** chunks;
  int chunk_count;
} dop_enc_test_ctx_t;

static void dop_enc_iter(int i, void* ctx) {
  dop_enc_test_ctx_t* c = (dop_enc_test_ctx_t*)ctx;
  dop_encoder_encode(c->encoder, c->chunks[i % c->chunk_count]);
}

TEST(DoPEncoder_AllocationFree) {
  dop_encoder_t* encoder =
      dop_encoder_create(2, 176400.0, true, SDM_FILTER_SDM4, 20000.0);
  ASSERT_TRUE(encoder != NULL);
  audio_chunk_t** inputs = make_random_chunks(32, 2, 1024, 0.5);
  dop_enc_test_ctx_t ctx = {encoder, inputs, 32};
  assert_allocation_free("DoP encoder", 0, 30, dop_enc_iter, &ctx);
  free_chunks(inputs, 32);
  dop_encoder_free(encoder);
}

typedef struct {
  dop_decoder_t* decoder;
  audio_chunk_t** chunks;
  int chunk_count;
} dop_dec_test_ctx_t;

static void dop_dec_iter(int i, void* ctx) {
  dop_dec_test_ctx_t* c = (dop_dec_test_ctx_t*)ctx;
  dop_decoder_detect_and_process(c->decoder, c->chunks[i % c->chunk_count]);
}

TEST(DoPDecoder_AllocationFree) {
  dop_decoder_t* decoder = dop_decoder_create(2, 176400.0, false, 20000.0);
  ASSERT_TRUE(decoder != NULL);
  int total_chunks = 36;
  audio_chunk_t** chunks =
      (audio_chunk_t**)calloc(total_chunks, sizeof(audio_chunk_t*));
  int global_frame_idx = 0;
  for (int i = 0; i < total_chunks; i++) {
    chunks[i] = audio_chunk_create(1024, 2);
    for (int t = 0; t < 1024; t++) {
      uint32_t marker = (global_frame_idx % 2 == 0) ? 0x05 : 0xFA;
      uint32_t val24 = (marker << 16) | 0x6969;
      int32_t int_val = (int32_t)(val24 << 8) >> 8;
      double f = (double)int_val / 8388608.0;
      audio_chunk_get_channel(chunks[i], 0)[t] = f;
      audio_chunk_get_channel(chunks[i], 1)[t] = f;
      global_frame_idx++;
    }
    audio_chunk_set_valid_frames(chunks[i], 1024);
  }
  dop_dec_test_ctx_t ctx = {decoder, chunks, total_chunks};
  assert_allocation_free("DoP decoder", 0, 30, dop_dec_iter, &ctx);
  free_chunks(chunks, total_chunks);
  dop_decoder_free(decoder);
}

static void logger_iter(int i, void* ctx) {
  (void)ctx;
  logger_t logger = logger_create("test.alloc.free");
  logger_info(&logger, "Test event: int=%d, float=%f, static=%s",
              log_arg_int((int64_t)i), log_arg_double(3.14159 + (double)i),
              log_arg_string("Static string argument value"), log_arg_none());
}

TEST(Logger_AllocationFree) {
  assert_allocation_free("Logger various arguments", 0, 30, logger_iter, NULL);
}

typedef struct {
  processing_parameters_t* params;
  audio_chunk_t** chunks;
  int chunk_count;
} proc_params_test_ctx_t;

static void proc_params_iter(int i, void* ctx) {
  proc_params_test_ctx_t* c = (proc_params_test_ctx_t*)ctx;
  processing_parameters_update_capture_levels(c->params,
                                              c->chunks[i % c->chunk_count]);
  processing_parameters_update_playback_levels(c->params,
                                               c->chunks[i % c->chunk_count]);
}

TEST(ProcessingParameters_AllocationFree) {
  processing_parameters_t* params = processing_parameters_create(2, 2);
  ASSERT_TRUE(params != NULL);
  audio_chunk_t** chunks = make_random_chunks(32, 2, 1024, 1.0);
  proc_params_test_ctx_t ctx = {params, chunks, 32};
  assert_allocation_free("ProcessingParameters updateLevels", 0, 30,
                         proc_params_iter, &ctx);
  free_chunks(chunks, 32);
  processing_parameters_free(params);
}

TEST_MAIN()
