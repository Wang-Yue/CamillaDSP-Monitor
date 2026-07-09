#if defined(__linux__)
#define _GNU_SOURCE
#endif
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../Sources/CDSP/Filters/biquad.h"
#include "../../Sources/CDSP/Filters/convolution.h"
#include "../../Sources/CDSP/Filters/diffeq.h"
#include "test_support.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CHUNK_SIZE 1024
#define SAMPLE_RATE 48000
#define NBR_FRAMES (16 * CHUNK_SIZE)

typedef struct {
  uint64_t state;
} seeded_rng_t;

static inline uint64_t seeded_rng_next(seeded_rng_t* rng) {
  rng->state = rng->state * 6364136223846793005ULL + 1442695040888963407ULL;
  return rng->state;
}

static inline double seeded_rng_random_double(seeded_rng_t* rng) {
  uint64_t val = seeded_rng_next(rng);
  double u = (double)(val >> 11) * (1.0 / 9007199254740992.0);  // 2^53
  return u * 2.0 - 1.0;
}

static void make_test_signal(double* x) {
  seeded_rng_t rng = {.state = 0xCDD5AA42DEADBEEFULL};
  const double f1 = 200.0;
  const double f2 = 1500.0;
  const double f3 = 8000.0;
  for (size_t i = 0; i < NBR_FRAMES; i++) {
    double t = (double)i / (double)SAMPLE_RATE;
    x[i] = 0.4 * sin(2.0 * M_PI * f1 * t) + 0.3 * sin(2.0 * M_PI * f2 * t) +
           0.2 * sin(2.0 * M_PI * f3 * t) +
           0.05 * seeded_rng_random_double(&rng);
  }
}

typedef struct {
  char name[64];
  double ns_per_frame;
} rust_bench_result_t;

static rust_bench_result_t rust_results[5];
static int rust_results_count = 0;

static void run_upstream_filter_benchmarks(void) {
  if (rust_results_count > 0) return;
  const char* home = getenv("HOME");
  if (!home) return;
  char cmd[1024];
  snprintf(cmd, sizeof(cmd),
           "cd %s/camilladsp && cargo bench --bench filters -- --sample-size "
           "10 --warm-up-time 0.3 --measurement-time 0.5 2>&1",
           home);
  FILE* fp = popen(cmd, "r");
  if (!fp) return;
  char line[1024];
  while (fgets(line, sizeof(line), fp)) {
    if (strstr(line, "time:")) {
      for (char* p = line; *p; p++) {
        if (*p == '[' || *p == ']') *p = ' ';
      }
      char name[128] = {0};
      char time_lbl[32] = {0};
      double val1 = 0, val2 = 0, val3 = 0;
      char unit[32] = {0};
      int count = sscanf(line, "%127s %31s %lf %31s %lf %31s %lf %31s", name,
                         time_lbl, &val1, unit, &val2, unit, &val3, unit);
      if (count >= 8 && strcmp(time_lbl, "time:") == 0) {
        double val_ns = val2;
        if (strcmp(unit, "µs") == 0 || strstr(unit, "u")) {
          val_ns = val2 * 1000.0;
        } else if (strcmp(unit, "ms") == 0) {
          val_ns = val2 * 1000000.0;
        }
        if (rust_results_count < 5) {
          strcpy(rust_results[rust_results_count].name, name);
          rust_results[rust_results_count].ns_per_frame = val_ns / 1024.0;
          rust_results_count++;
        }
      }
    }
  }
  pclose(fp);
}

static double get_rust_result(const char* name) {
  run_upstream_filter_benchmarks();
  for (int i = 0; i < rust_results_count; i++) {
    if (strcmp(rust_results[i].name, name) == 0) {
      return rust_results[i].ns_per_frame;
    }
  }
  return NAN;
}

static void run_filter_benchmark(const char* label, const char* rust_name,
                                 void* filter,
                                 void (*process_fn)(void*, double*, size_t)) {
  double* buffer = (double*)calloc(CHUNK_SIZE, sizeof(double));

  // Warm-up
  for (int i = 0; i < 100; i++) {
    process_fn(filter, buffer, CHUNK_SIZE);
  }

  int iters = 5000;
  struct timespec start, end_time;
  clock_gettime(CLOCK_MONOTONIC, &start);
  for (int i = 0; i < iters; i++) {
    process_fn(filter, buffer, CHUNK_SIZE);
  }
  clock_gettime(CLOCK_MONOTONIC, &end_time);

  double elapsed_ns = (double)(end_time.tv_sec - start.tv_sec) * 1e9 +
                      (double)(end_time.tv_nsec - start.tv_nsec);
  double c_ns_per_frame = elapsed_ns / (double)(CHUNK_SIZE * iters);

  double cdsp_ns_per_frame = get_rust_result(rust_name);

  printf("\n==================================================\n");
  printf("Filter Benchmark: %s\n", label);
  printf("--------------------------------------------------\n");
  printf("Engine                   |        ns/frame\n");
  printf("--------------------------------------------------\n");
  printf("C %-22s | %15.1f\n", label, c_ns_per_frame);
  if (!isnan(cdsp_ns_per_frame)) {
    printf("CamillaDSP (Rust)        | %15.1f\n", cdsp_ns_per_frame);
  } else {
    printf("CamillaDSP (Rust)        |             N/A\n");
  }
  printf("--------------------------------------------------\n");
  if (!isnan(cdsp_ns_per_frame)) {
    printf("Relative Speedup        : %14.2fx\n",
           cdsp_ns_per_frame / c_ns_per_frame);
  }
  printf("==================================================\n\n");

  free(buffer);
}

static void process_conv(void* f, double* w, size_t n) {
  convolution_filter_process((convolution_filter_t*)f, w, n);
}

static void process_biquad(void* f, double* w, size_t n) {
  biquad_filter_process((biquad_filter_t*)f, w, n);
}

static void process_diffeq(void* f, double* w, size_t n) {
  diffeq_filter_process((diffeq_filter_t*)f, w, n);
}

TEST(Convolution_1024_Benchmark) {
  double* coeffs = (double*)calloc(1024, sizeof(double));
  conv_parameters_t params = {
      .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 1024};
  convolution_filter_t* f =
      convolution_filter_create("conv-1024", &params, CHUNK_SIZE);
  run_filter_benchmark("FftConv_1024", "Conv/FftConv/1024", f, process_conv);
  convolution_filter_free(f);
  free(coeffs);
}

TEST(Convolution_4096_Benchmark) {
  double* coeffs = (double*)calloc(4096, sizeof(double));
  conv_parameters_t params = {
      .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 4096};
  convolution_filter_t* f =
      convolution_filter_create("conv-4096", &params, CHUNK_SIZE);
  run_filter_benchmark("FftConv_4096", "Conv/FftConv/4096", f, process_conv);
  convolution_filter_free(f);
  free(coeffs);
}

TEST(Convolution_16384_Benchmark) {
  double* coeffs = (double*)calloc(16384, sizeof(double));
  conv_parameters_t params = {
      .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 16384};
  convolution_filter_t* f =
      convolution_filter_create("conv-16384", &params, CHUNK_SIZE);
  run_filter_benchmark("FftConv_16384", "Conv/FftConv/16384", f, process_conv);
  convolution_filter_free(f);
  free(coeffs);
}

TEST(Biquad_Benchmark) {
  biquad_coefficients_t coeffs = {.b0 = 0.21476322779271284,
                                  .b1 = 0.4295264555854257,
                                  .b2 = 0.21476322779271284,
                                  .a1 = -0.1462978543780541,
                                  .a2 = 0.005350765548905586};
  biquad_filter_t* f = biquad_filter_create("biquad", &coeffs);
  run_filter_benchmark("Biquad", "Biquad", f, process_biquad);
  biquad_filter_free(f);
}

TEST(DiffEq_Benchmark) {
  double a[] = {1.0, -0.1462978543780541, 0.005350765548905586};
  double b[] = {0.21476322779271284, 0.4295264555854257, 0.21476322779271284};
  diff_eq_parameters_t params = {.a = a, .a_count = 3, .b = b, .b_count = 3};
  diffeq_filter_t* f = diffeq_filter_create("diffeq", &params);
  run_filter_benchmark("DiffEq", "DiffEq", f, process_diffeq);
  diffeq_filter_free(f);
}

TEST_MAIN()
