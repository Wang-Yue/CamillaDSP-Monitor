#ifndef _WIN32
#include <dlfcn.h>
#endif
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
#include "../../Sources/CDSP/Engine/engine_processing_loop.h"
#include "../../Sources/CDSP/Engine/engine_shared_state.h"
#include "../../Sources/CDSP/Engine/engine_state_machine.h"
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
#include "../../Sources/CDSP/Pipeline/pipeline.h"
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

static malloc_logger_t g_custom_malloc_logger = NULL;

#ifdef __linux__
#include <unistd.h>

static void* (*real_malloc)(size_t) = NULL;
static void* (*real_calloc)(size_t, size_t) = NULL;
static void* (*real_realloc)(void*, size_t) = NULL;
static void (*real_free)(void*) = NULL;

static char bootstrap_buffer[8192];
static size_t bootstrap_offset = 0;
static _Thread_local bool in_bootstrap = false;

static void* bootstrap_malloc(size_t size) {
  size_t aligned_size = (size + 15) & ~15;
  if (bootstrap_offset + aligned_size > sizeof(bootstrap_buffer)) {
    const char* msg = "FATAL: out of bootstrap memory in malloc wrapper\n";
    write(2, msg, strlen(msg));
    abort();
  }
  void* ptr = bootstrap_buffer + bootstrap_offset;
  bootstrap_offset += aligned_size;
  return ptr;
}
static void init_real_allocators(void) {
  if (real_malloc) return;
  if (in_bootstrap) return;
  in_bootstrap = true;
  real_malloc = (void* (*)(size_t))dlsym(RTLD_NEXT, "malloc");
  real_calloc = (void* (*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
  real_realloc = (void* (*)(void*, size_t))dlsym(RTLD_NEXT, "realloc");
  real_free = (void (*)(void*))dlsym(RTLD_NEXT, "free");
  in_bootstrap = false;
  if (!real_malloc || !real_calloc || !real_realloc || !real_free) {
    const char* msg = "FATAL: failed to find real allocators via dlsym\n";
    write(2, msg, strlen(msg));
    abort();
  }
}

void* malloc(size_t size) {
  if (!real_malloc) {
    init_real_allocators();
    if (!real_malloc) return bootstrap_malloc(size);
  }
  void* ptr = real_malloc(size);
  malloc_logger_t logger = atomic_load_explicit(
      (_Atomic malloc_logger_t*)&g_custom_malloc_logger, memory_order_acquire);
  if (logger) {
    logger(2, 0, (uintptr_t)size, 0, (uintptr_t)ptr, 0);
  }
  return ptr;
}

void* calloc(size_t num, size_t size) {
  size_t total = num * size;
  if (!real_calloc) {
    init_real_allocators();
    if (!real_calloc) {
      void* ptr = bootstrap_malloc(total);
      if (ptr) memset(ptr, 0, total);
      return ptr;
    }
  }
  void* ptr = real_calloc(num, size);
  malloc_logger_t logger = atomic_load_explicit(
      (_Atomic malloc_logger_t*)&g_custom_malloc_logger, memory_order_acquire);
  if (logger) {
    logger(2, 0, (uintptr_t)total, 0, (uintptr_t)ptr, 0);
  }
  return ptr;
}

void* realloc(void* ptr, size_t size) {
  if (ptr >= (void*)bootstrap_buffer &&
      ptr < (void*)(bootstrap_buffer + sizeof(bootstrap_buffer))) {
    void* new_ptr = bootstrap_malloc(size);
    if (new_ptr && ptr) {
      memcpy(new_ptr, ptr, size);
    }
    return new_ptr;
  }
  if (!real_realloc) {
    init_real_allocators();
  }
  void* new_ptr = real_realloc(ptr, size);
  malloc_logger_t logger = atomic_load_explicit(
      (_Atomic malloc_logger_t*)&g_custom_malloc_logger, memory_order_acquire);
  if (logger) {
    logger(2, 0, (uintptr_t)size, 0, (uintptr_t)new_ptr, 0);
  }
  return new_ptr;
}

void free(void* ptr) {
  if (!ptr) return;
  if (ptr >= (void*)bootstrap_buffer &&
      ptr < (void*)(bootstrap_buffer + sizeof(bootstrap_buffer))) {
    return;
  }
  if (!real_free) {
    init_real_allocators();
  }
  if (real_free) {
    real_free(ptr);
  }
  malloc_logger_t logger = atomic_load_explicit(
      (_Atomic malloc_logger_t*)&g_custom_malloc_logger, memory_order_acquire);
  if (logger) {
    logger(4, 0, 0, 0, (uintptr_t)ptr, 0);
  }
}
#endif  // __linux__

#ifdef _WIN32
// Declarations of real functions (resolved by linker)
void* __real_malloc(size_t size);
void* __real_calloc(size_t num, size_t size);
void* __real_realloc(void* ptr, size_t size);
void __real_free(void* ptr);

void* __wrap_malloc(size_t size) {
  void* ptr = __real_malloc(size);
  malloc_logger_t logger = atomic_load_explicit(
      (_Atomic malloc_logger_t*)&g_custom_malloc_logger, memory_order_acquire);
  if (logger) {
    logger(2, 0, (uintptr_t)size, 0, (uintptr_t)ptr, 0);
  }
  return ptr;
}

void* __wrap_calloc(size_t num, size_t size) {
  size_t total = num * size;
  void* ptr = __real_calloc(num, size);
  malloc_logger_t logger = atomic_load_explicit(
      (_Atomic malloc_logger_t*)&g_custom_malloc_logger, memory_order_acquire);
  if (logger) {
    logger(2, 0, (uintptr_t)total, 0, (uintptr_t)ptr, 0);
  }
  return ptr;
}

void* __wrap_realloc(void* ptr, size_t size) {
  void* new_ptr = __real_realloc(ptr, size);
  malloc_logger_t logger = atomic_load_explicit(
      (_Atomic malloc_logger_t*)&g_custom_malloc_logger, memory_order_acquire);
  if (logger) {
    logger(2, 0, (uintptr_t)size, 0, (uintptr_t)new_ptr, 0);
  }
  return new_ptr;
}

void __wrap_free(void* ptr) {
  __real_free(ptr);
  malloc_logger_t logger = atomic_load_explicit(
      (_Atomic malloc_logger_t*)&g_custom_malloc_logger, memory_order_acquire);
  if (logger) {
    logger(4, 0, 0, 0, (uintptr_t)ptr, 0);
  }
}
#endif  // _WIN32

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
#if defined(__linux__) || defined(_WIN32)
  uintptr_t my_thread = (uintptr_t)pthread_self();
  atomic_store_explicit(&g_alloc_counter, 0, memory_order_relaxed);
  atomic_store_explicit(&g_watched_thread, my_thread, memory_order_release);
  atomic_store_explicit((_Atomic malloc_logger_t*)&g_custom_malloc_logger,
                        my_malloc_logger, memory_order_release);

  body(ctx);

  atomic_store_explicit((_Atomic malloc_logger_t*)&g_custom_malloc_logger, NULL,
                        memory_order_release);
  atomic_store_explicit(&g_watched_thread, 0, memory_order_release);
  *out_count = atomic_load_explicit(&g_alloc_counter, memory_order_relaxed);
  return true;
#else  // macOS
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
#endif
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
  ASSERT_EQ(0, count);
}

static bool count_allocations_on_thread(void (*body)(void*), void* ctx,
                                        uintptr_t thread_id,
                                        uint64_t* out_count) {
#if defined(__linux__) || defined(_WIN32)
  atomic_store_explicit(&g_alloc_counter, 0, memory_order_relaxed);
  atomic_store_explicit(&g_watched_thread, thread_id, memory_order_release);
  atomic_store_explicit((_Atomic malloc_logger_t*)&g_custom_malloc_logger,
                        my_malloc_logger, memory_order_release);

  body(ctx);

  atomic_store_explicit((_Atomic malloc_logger_t*)&g_custom_malloc_logger, NULL,
                        memory_order_release);
  atomic_store_explicit(&g_watched_thread, 0, memory_order_release);
  *out_count = atomic_load_explicit(&g_alloc_counter, memory_order_relaxed);
  return true;
#else  // macOS
  void* handle = dlopen(NULL, RTLD_LAZY);
  if (!handle) return false;
  malloc_logger_t* logger_ptr =
      (malloc_logger_t*)dlsym(handle, "malloc_logger");
  if (!logger_ptr) {
    dlclose(handle);
    return false;
  }

  g_prev_logger = *logger_ptr;
  atomic_store_explicit(&g_alloc_counter, 0, memory_order_relaxed);
  atomic_store_explicit(&g_watched_thread, thread_id, memory_order_release);
  *logger_ptr = my_malloc_logger;

  body(ctx);

  *logger_ptr = g_prev_logger;
  atomic_store_explicit(&g_watched_thread, 0, memory_order_release);
  *out_count = atomic_load_explicit(&g_alloc_counter, memory_order_relaxed);
  dlclose(handle);
  return true;
#endif
}

static void assert_allocation_free_on_thread(const char* label,
                                             uintptr_t thread_id, int warmup,
                                             int iterations,
                                             test_iter_func_t body, void* ctx) {
  for (int i = 0; i < warmup; i++) {
    body(i, ctx);
  }
  loop_ctx_t lctx = {body, warmup, iterations, ctx};
  uint64_t count = 0;
  if (!count_allocations_on_thread(run_test_loop, &lctx, thread_id, &count)) {
    printf("malloc_logger unavailable — %s skipped\n", label);
    return;
  }
  printf("[%s] allocations=%llu over %d iterations\n", label,
         (unsigned long long)count, iterations);
  ASSERT_EQ(0, count);
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
      audio_resampler_create_from_config(&cfg, 44100, 48000, 2, 1024, NULL);
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
      audio_resampler_create_from_config(&cfg, 44100, 48000, 2, 1024, NULL);
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
      audio_resampler_create_from_config(&cfg, 44100, 48000, 2, 1024, NULL);
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
      audio_resampler_create_from_config(&cfg, 44100, 48000, 2, 1024, NULL);
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
  biquad_filter_t* filter = biquad_filter_create("bq", &coeffs, NULL);
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
      convolution_filter_create("conv", &params, chunk_size, NULL);
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
      volume_filter_create("vol", &params, 44100, 1024, proc_params, NULL);
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
      loudness_filter_create("loud", &params, 44100, proc_params, NULL);
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
  delay_filter_t* filter = delay_filter_create("del", &params, 44100, NULL);
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
      biquad_combo_filter_create("combo", &params, 44100, NULL);
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
  race_processor_t* proc = race_processor_create("race", &params, 44100, NULL);
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

#if defined(ENABLE_BLAS)
void openblas_set_num_threads(int num_threads);
#endif

TEST(DoPEncoder_AllocationFree) {
#if defined(ENABLE_BLAS)
  openblas_set_num_threads(1);
#endif
  dop_encoder_t* encoder =
      dop_encoder_create(2, 176400.0, true, SDM_FILTER_SDM4, 20000.0);
  ASSERT_TRUE(encoder != NULL);
  audio_chunk_t** inputs = make_random_chunks(32, 2, 1024, 0.5);
  dop_enc_test_ctx_t ctx = {encoder, inputs, 32};
  assert_allocation_free("DoP encoder",
#if defined(ENABLE_BLAS)
                         1,
#else
                         0,
#endif
                         30, dop_enc_iter, &ctx);
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
  assert_allocation_free("Logger various arguments", 1, 30, logger_iter, NULL);
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

typedef struct {
  engine_processing_loop_t* loop;
  engine_shared_state_t* shared;
  pipeline_t** reloaded_pipelines;
  audio_chunk_t* input_chunk;
  engine_semaphore_t thread_id_sem;
  engine_semaphore_t processed_sem;
  _Atomic uintptr_t watched_thread_id;
} pipeline_reload_test_ctx_t;

static void on_chunk_captured_cb(void* ctx, const audio_chunk_t* chunk) {
  (void)ctx;
  (void)chunk;
}

static void on_chunk_processed_cb(void* ctx, const audio_chunk_t* chunk) {
  (void)chunk;
  pipeline_reload_test_ctx_t* c = (pipeline_reload_test_ctx_t*)ctx;
  uintptr_t tid = (uintptr_t)pthread_self();
  uintptr_t expected = 0;
  if (atomic_compare_exchange_strong(&c->watched_thread_id, &expected, tid)) {
    engine_sem_signal(c->thread_id_sem);
  }
  engine_sem_signal(c->processed_sem);
}

static void* test_processing_thread_run(void* arg) {
  engine_processing_loop_run((engine_processing_loop_t*)arg);
  return NULL;
}

static void reload_iter_c(int i, void* ctx) {
  pipeline_reload_test_ctx_t* c = (pipeline_reload_test_ctx_t*)ctx;

  // 1. Enqueue chunk
  spsc_queue_enqueue(c->shared->captured_queue, c->input_chunk);

  // 2. Set pipeline (i + 1 because 0 was used in warmup)
  engine_processing_loop_set_pipeline(c->loop, c->reloaded_pipelines[i + 1]);

  // 3. Signal captured semaphore
  engine_sem_signal(c->shared->captured_semaphore);

  // 4. Wait for processing completion
  engine_sem_wait(c->processed_sem);

  // 5. Clean up queues
  void* processed = spsc_queue_dequeue(c->shared->processed_queue);
  (void)processed;

  void* garbage = NULL;
  while ((garbage = spsc_queue_dequeue(c->shared->pipeline_garbage_queue)) !=
         NULL) {
    pipeline_free((pipeline_t*)garbage);
  }
}

static void init_default_config(dsp_config_t* config) {
  memset(config, 0, sizeof(dsp_config_t));
  config->devices.samplerate = 44100;
  config->devices.chunksize = 1024;
#if defined(ENABLE_COREAUDIO)
  config->devices.capture.type = AUDIO_BACKEND_TYPE_CORE_AUDIO;
  config->devices.capture.cfg.coreaudio.channels = 2;
  config->devices.playback.type = AUDIO_BACKEND_TYPE_CORE_AUDIO;
  config->devices.playback.cfg.coreaudio.channels = 2;
#elif defined(ENABLE_ALSA)
  config->devices.capture.type = AUDIO_BACKEND_TYPE_ALSA;
  config->devices.capture.cfg.alsa.channels = 2;
  config->devices.playback.type = AUDIO_BACKEND_TYPE_ALSA;
  config->devices.playback.cfg.alsa.channels = 2;
#elif defined(ENABLE_WASAPI)
  config->devices.capture.type = AUDIO_BACKEND_TYPE_WASAPI;
  config->devices.capture.cfg.wasapi.channels = 2;
  config->devices.playback.type = AUDIO_BACKEND_TYPE_WASAPI;
  config->devices.playback.cfg.wasapi.channels = 2;
#else
  config->devices.capture.type = AUDIO_BACKEND_TYPE_FILE;
  config->devices.capture.cfg.raw_file.channels = 2;
  config->devices.playback.type = AUDIO_BACKEND_TYPE_FILE;
  config->devices.playback.cfg.raw_file.channels = 2;
#endif
}

TEST(PipelineReload_AllocationFree) {
  dsp_config_t config;
  init_default_config(&config);

  processing_parameters_t* params = processing_parameters_create(2, 2);
  pipeline_t* initial_pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(initial_pipeline != NULL);

  // Pre-create 30 pipelines
  pipeline_t* reloaded_pipelines[30];
  for (int i = 0; i < 30; i++) {
    reloaded_pipelines[i] = pipeline_create(&config, params, 0, NULL);
    ASSERT_TRUE(reloaded_pipelines[i] != NULL);
  }

  engine_shared_state_t* shared = engine_shared_state_create(32, 32);
  ASSERT_TRUE(shared != NULL);

  engine_state_machine_t* state_machine = engine_state_machine_create();
  engine_state_machine_set_state(state_machine, PROCESSING_STATE_RUNNING);

  audio_chunk_t* resampler_scratch = audio_chunk_create(1024, 2);
  audio_chunk_t* pipeline_scratch = audio_chunk_create(1024, 2);

  round_robin_chunk_pool_t* scratch_pool =
      round_robin_chunk_pool_create(32, 1024, 2);

  pipeline_reload_test_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.shared = shared;
  ctx.reloaded_pipelines = reloaded_pipelines;
  ctx.input_chunk = audio_chunk_create(1024, 2);
  audio_chunk_set_valid_frames(ctx.input_chunk, 1024);

  engine_sem_init(&ctx.thread_id_sem);
  engine_sem_init(&ctx.processed_sem);
  atomic_init(&ctx.watched_thread_id, 0);

  engine_processing_loop_t* loop = engine_processing_loop_create(
      shared, state_machine, params, 44100,
      NULL,  // resampler
      initial_pipeline,
      NULL,  // dop_encoder
      resampler_scratch, pipeline_scratch, scratch_pool, on_chunk_captured_cb,
      NULL, on_chunk_processed_cb, &ctx);
  ASSERT_TRUE(loop != NULL);
  ctx.loop = loop;

  // Spawn processing loop thread
  pthread_t thread;
  pthread_create(&thread, NULL, test_processing_thread_run, loop);

  // Warmup / get thread ID
  spsc_queue_enqueue(shared->captured_queue, ctx.input_chunk);
  engine_processing_loop_set_pipeline(loop, reloaded_pipelines[0]);
  engine_sem_signal(shared->captured_semaphore);

  engine_sem_wait(ctx.thread_id_sem);
  engine_sem_wait(ctx.processed_sem);

  // Clean up queues after warmup
  void* processed = spsc_queue_dequeue(shared->processed_queue);
  (void)processed;
  void* garbage = NULL;
  while ((garbage = spsc_queue_dequeue(shared->pipeline_garbage_queue)) !=
         NULL) {
    pipeline_free((pipeline_t*)garbage);
  }

  uintptr_t tid = atomic_load(&ctx.watched_thread_id);

  // Run measured iterations
  assert_allocation_free_on_thread("Pipeline C hot reload", tid, 0, 20,
                                   reload_iter_c, &ctx);

  // Stop the thread
  atomic_store_explicit(&shared->should_stop, true, memory_order_release);
  engine_sem_signal(shared->captured_semaphore);
  pthread_join(thread, NULL);

  // Cleanup
  engine_processing_loop_free(loop);
  for (int i = 21; i < 30; i++) {
    pipeline_free(reloaded_pipelines[i]);
  }

  audio_chunk_free(ctx.input_chunk);
  audio_chunk_free(resampler_scratch);
  audio_chunk_free(pipeline_scratch);
  round_robin_chunk_pool_free(scratch_pool);
  engine_shared_state_free(shared);
  engine_state_machine_free(state_machine);
  processing_parameters_free(params);

  engine_sem_destroy(&ctx.thread_id_sem);
  engine_sem_destroy(&ctx.processed_sem);
}

typedef struct {
  pipeline_t* pipeline;
  audio_chunk_t* input;
  audio_chunk_t* output;
} pipeline_test_ctx_t;

static void pipeline_iter(int i, void* ctx) {
  (void)i;
  pipeline_test_ctx_t* c = (pipeline_test_ctx_t*)ctx;
  pipeline_error_t err = pipeline_process(c->pipeline, c->input, c->output);
  (void)err;
}

TEST(Pipeline_AllocationFree) {
  dsp_config_t config;
  memset(&config, 0, sizeof(dsp_config_t));
  config.devices.samplerate = 48000;
  config.devices.chunksize = 1024;
  config.devices.capture.type = AUDIO_BACKEND_TYPE_FILE;
  config.devices.capture.cfg.raw_file.channels = 4;
  config.devices.playback.type = AUDIO_BACKEND_TYPE_FILE;
  config.devices.playback.cfg.raw_file.channels = 2;

  named_filter_config_t filters[10];
  memset(filters, 0, sizeof(filters));

  for (int i = 0; i < 8; i++) {
    sprintf(filters[i].name, "bq_%d", i + 1);
    filters[i].filter.type = FILTER_TYPE_BIQUAD;
    filters[i].filter.parameters.biquad.type = BIQUAD_TYPE_PEAKING;
    filters[i].filter.parameters.biquad.freq = 1000.0 * (i + 1);
    filters[i].filter.parameters.biquad.q = 0.707;
    filters[i].filter.parameters.biquad.gain = 1.0;
  }

  double ir[1024];
  for (int i = 0; i < 1024; i++) {
    ir[i] = i == 0 ? 1.0 : 0.0;
  }

  strcpy(filters[8].name, "conv_1");
  filters[8].filter.type = FILTER_TYPE_CONV;
  filters[8].filter.parameters.conv.type = CONV_TYPE_VALUES;
  filters[8].filter.parameters.conv.values = ir;
  filters[8].filter.parameters.conv.values_count = 1024;

  strcpy(filters[9].name, "conv_2");
  filters[9].filter.type = FILTER_TYPE_CONV;
  filters[9].filter.parameters.conv.type = CONV_TYPE_VALUES;
  filters[9].filter.parameters.conv.values = ir;
  filters[9].filter.parameters.conv.values_count = 1024;

  config.filters = filters;
  config.filters_count = 10;

  mixer_source_t src0[2] = {
      {.channel = 0, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB},
      {.channel = 2, .gain = -6.0, .has_gain = true, .scale = GAIN_SCALE_DB}};
  mixer_source_t src1[2] = {
      {.channel = 1, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB},
      {.channel = 3, .gain = -6.0, .has_gain = true, .scale = GAIN_SCALE_DB}};
  mixer_mapping_t maps[2] = {
      {.dest = 0, .sources_count = 2, .sources = src0, .mute = false},
      {.dest = 1, .sources_count = 2, .sources = src1, .mute = false}};
  named_mixer_config_t mixer_cfg;
  memset(&mixer_cfg, 0, sizeof(mixer_cfg));
  strcpy(mixer_cfg.name, "mix");
  mixer_cfg.mixer.channels_in = 4;
  mixer_cfg.mixer.channels_out = 2;
  mixer_cfg.mixer.mapping_count = 2;
  mixer_cfg.mixer.mapping = maps;

  config.mixers = &mixer_cfg;
  config.mixers_count = 1;

  pipeline_step_t steps[3];
  memset(steps, 0, sizeof(steps));

  steps[0].type = PIPELINE_STEP_TYPE_FILTER;
  steps[0].has_channel = false;
  char* pre_names[10];
  for (int i = 0; i < 10; i++) {
    pre_names[i] = filters[i].name;
  }
  steps[0].names = pre_names;
  steps[0].names_count = 10;

  steps[1].type = PIPELINE_STEP_TYPE_MIXER;
  strcpy(steps[1].name, "mix");
  steps[1].has_name = true;

  steps[2].type = PIPELINE_STEP_TYPE_FILTER;
  steps[2].has_channel = false;
  char* post_names[10];
  for (int i = 0; i < 10; i++) {
    post_names[i] = filters[i].name;
  }
  steps[2].names = post_names;
  steps[2].names_count = 10;

  config.pipeline = steps;
  config.pipeline_count = 3;

  processing_parameters_t* params = processing_parameters_create(4, 2);
  ASSERT_TRUE(params != NULL);

  pipeline_t* pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  audio_chunk_t* input = audio_chunk_create(1024, 4);
  audio_chunk_t* output = audio_chunk_create(1024, 2);

  for (size_t ch = 0; ch < 4; ch++) {
    mutable_waveform_t w = audio_chunk_get_channel(input, ch);
    for (size_t t = 0; t < 1024; t++) {
      w[t] = 0.05 * (double)(t % 20 - 10);
    }
  }
  audio_chunk_set_valid_frames(input, 1024);

  pipeline_test_ctx_t ctx = {pipeline, input, output};
  assert_allocation_free("Pipeline C (Single-Threaded)", 0, 30, pipeline_iter,
                         &ctx);

  audio_chunk_free(input);
  audio_chunk_free(output);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST_MAIN()
