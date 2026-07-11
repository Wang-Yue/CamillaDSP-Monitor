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

#include "../../Sources/CDSP/Pipeline/config_loader.h"
#include "../../Sources/CDSP/Pipeline/pipeline.h"
#include "test_support.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CHUNK_SIZE 1024
#define SAMPLE_RATE 48000
#define ITERS 200

static const double pre_bq_freqs[] = {
    120.0,  220.0,  350.0,  500.0,  700.0,  900.0,  1200.0, 1600.0,
    1800.0, 2200.0, 2800.0, 3200.0, 3800.0, 4500.0, 6200.0, 8000.0};
static const double pre_bq_qs[] = {0.70, 0.75, 0.80, 0.90, 1.00, 1.10,
                                   0.95, 1.05, 1.10, 0.90, 0.95, 1.00,
                                   0.85, 0.80, 0.75, 0.70};

static const double post_bq_freqs[] = {
    140.0,  260.0,  400.0,  560.0,  760.0,  980.0,  1300.0, 1700.0,
    2100.0, 2500.0, 3000.0, 3600.0, 4200.0, 5200.0, 6800.0, 9200.0};
static const double post_bq_qs[] = {0.72, 0.78, 0.83, 0.92, 1.02, 1.08,
                                    0.98, 1.06, 1.00, 0.94, 0.92, 0.88,
                                    0.84, 0.80, 0.76, 0.72};

static audio_chunk_t* make_dummy_signal(int channels) {
  audio_chunk_t* chunk = audio_chunk_create(CHUNK_SIZE, channels);
  for (int ch = 0; ch < channels; ch++) {
    mutable_waveform_t w = audio_chunk_get_channel(chunk, ch);
    for (int t = 0; t < CHUNK_SIZE; t++) {
      w[t] = sin(2.0 * M_PI * 1000.0 * (double)t / (double)SAMPLE_RATE);
    }
  }
  audio_chunk_set_valid_frames(chunk, CHUNK_SIZE);
  return chunk;
}

static double* build_conv_filter_coefficients(int length) {
  double* values = (double*)malloc(length * sizeof(double));
  for (int idx = 0; idx < length; idx++) {
    double x = (double)idx - (double)(length - 1) * 0.5;
    double sinc = (x == 0.0) ? 1.0 : sin(M_PI * x) / (M_PI * x);
    values[idx] = sinc;
  }
  return values;
}

typedef struct {
  double single_ms;
  double multi_ms;
} pipeline_rust_results_t;

static pipeline_rust_results_t run_upstream_pipeline_benchmark(
    const char* variant_single, const char* variant_multi) {
  pipeline_rust_results_t results = {.single_ms = NAN, .multi_ms = NAN};
  const char* home = getenv("HOME");
  if (!home) return results;

  char cmd[1024];
  snprintf(cmd, sizeof(cmd),
           "cd %s/camilladsp && cargo bench --bench pipeline -- --sample-size "
           "10 --warm-up-time 0.3 --measurement-time 0.5 2>&1",
           home);
  FILE* fp = popen(cmd, "r");
  if (!fp) return results;

  char line[1024];
  char last_variant[128] = {0};
  while (fgets(line, sizeof(line), fp)) {
    if (strstr(line, "complete_pipeline_chunk/variant/")) {
      char* p = strrchr(line, '/');
      if (p) {
        char* end = p + 1;
        while (*end && *end != '\n' && *end != '\r' && *end != ' ') end++;
        size_t len = end - (p + 1);
        if (len < sizeof(last_variant)) {
          strncpy(last_variant, p + 1, len);
          last_variant[len] = '\0';
        }
      }
    } else if (strstr(line, "time:") && last_variant[0] != '\0') {
      for (char* p = line; *p; p++) {
        if (*p == '[' || *p == ']') *p = ' ';
      }
      char* time_ptr = strstr(line, "time:");
      double val1 = 0, val2 = 0, val3 = 0;
      char unit[32] = {0};
      int count = sscanf(time_ptr, "time: %lf %31s %lf %31s %lf %31s", &val1,
                         unit, &val2, unit, &val3, unit);
      if (count >= 4) {
        double val_ms = val2;
        if (strcmp(unit, "µs") == 0 || strstr(unit, "u")) {
          val_ms = val2 / 1000.0;
        } else if (strcmp(unit, "ns") == 0) {
          val_ms = val2 / 1000000.0;
        }

        if (strcmp(last_variant, variant_single) == 0) {
          results.single_ms = val_ms;
        } else if (strcmp(last_variant, variant_multi) == 0) {
          results.multi_ms = val_ms;
        }
      }
      last_variant[0] = '\0';
    }
  }
  pclose(fp);
  return results;
}

static void print_comparison_table(const char* label, double rust_single,
                                   double rust_multi, double c_single,
                                   double c_multi) {
  printf(
      "\n======================================================================"
      "==========\n");
  printf("Benchmark: %s\n", label);
  printf(
      "------------------------------------------------------------------------"
      "--------\n");
  printf("Engine            |  Single (ms) |   Multi (ms) |    Speedup\n");
  printf(
      "------------------------------------------------------------------------"
      "--------\n");

  if (!isnan(rust_single)) {
    printf("CamillaDSP (Rust) | %12.3f | %12.3f | %10.2fx\n", rust_single,
           rust_multi, rust_single / rust_multi);
  } else {
    printf("CamillaDSP (Rust) |          N/A |          N/A |        N/A\n");
  }

  printf("Pipeline C (CDSP) | %12.3f | %12.3f | %10.2fx\n", c_single, c_multi,
         c_single / c_multi);
  printf(
      "------------------------------------------------------------------------"
      "--------\n");

  if (!isnan(rust_multi)) {
    const char* winner;
    double factor;
    if (c_multi < rust_multi) {
      winner = "Pipeline C (CDSP)";
      factor = rust_multi / c_multi;
    } else {
      winner = "CamillaDSP (Rust)";
      factor = c_multi / rust_multi;
    }
    printf("Head-to-Head      | %s is %.2fx faster in multi-threaded mode\n",
           winner, factor);
  }
  printf(
      "========================================================================"
      "========\n\n");
}

TEST(Pipeline_Biquads_Benchmark) {
  dsp_config_t config;
  memset(&config, 0, sizeof(dsp_config_t));
  config.devices.samplerate = SAMPLE_RATE;
  config.devices.chunksize = CHUNK_SIZE;
  config.devices.capture.type = AUDIO_BACKEND_TYPE_FILE;
  config.devices.capture.cfg.raw_file.channels = 4;
  config.devices.playback.type = AUDIO_BACKEND_TYPE_FILE;
  config.devices.playback.cfg.raw_file.channels = 2;

  named_filter_config_t filters[32];
  memset(filters, 0, sizeof(filters));

  char* pre_names[16];
  for (int i = 0; i < 16; i++) {
    sprintf(filters[i].name, "pre_bq_%d", i + 1);
    filters[i].filter.type = FILTER_TYPE_BIQUAD;
    filters[i].filter.parameters.biquad.type = BIQUAD_TYPE_PEAKING;
    filters[i].filter.parameters.biquad.freq = pre_bq_freqs[i];
    filters[i].filter.parameters.biquad.q = pre_bq_qs[i];
    filters[i].filter.parameters.biquad.gain = 1.5;
    pre_names[i] = filters[i].name;
  }

  char* post_names[16];
  for (int i = 0; i < 16; i++) {
    sprintf(filters[16 + i].name, "post_bq_%d", i + 1);
    filters[16 + i].filter.type = FILTER_TYPE_BIQUAD;
    filters[16 + i].filter.parameters.biquad.type = BIQUAD_TYPE_PEAKING;
    filters[16 + i].filter.parameters.biquad.freq = post_bq_freqs[i];
    filters[16 + i].filter.parameters.biquad.q = post_bq_qs[i];
    filters[16 + i].filter.parameters.biquad.gain = 1.5;
    post_names[i] = filters[16 + i].name;
  }

  config.filters = filters;
  config.filters_count = 32;

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
  strcpy(mixer_cfg.name, "mix_4_to_2");
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
  steps[0].names = pre_names;
  steps[0].names_count = 16;

  steps[1].type = PIPELINE_STEP_TYPE_MIXER;
  strcpy(steps[1].name, "mix_4_to_2");
  steps[1].has_name = true;

  steps[2].type = PIPELINE_STEP_TYPE_FILTER;
  steps[2].has_channel = false;
  steps[2].names = post_names;
  steps[2].names_count = 16;

  config.pipeline = steps;
  config.pipeline_count = 3;

  processing_parameters_t* params = processing_parameters_create(4, 2);
  ASSERT_TRUE(params != NULL);

  // Measure Single-Threaded C
  config.devices.multithreaded = false;
  config.devices.has_multithreaded = true;
  pipeline_t* pipeline_single = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline_single != NULL);

  // Measure Multi-Threaded C
  config.devices.multithreaded = true;
  config.devices.has_multithreaded = true;
  pipeline_t* pipeline_multi = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline_multi != NULL);

  audio_chunk_t* input = make_dummy_signal(4);
  audio_chunk_t* output = audio_chunk_create(CHUNK_SIZE, 2);

  // Warm-up
  for (int i = 0; i < 50; i++) {
    pipeline_process(pipeline_single, input, output);
    pipeline_process(pipeline_multi, input, output);
  }

  // Benchmark Single
  struct timespec start_single, end_single;
  clock_gettime(CLOCK_MONOTONIC, &start_single);
  for (int i = 0; i < ITERS; i++) {
    pipeline_process(pipeline_single, input, output);
  }
  clock_gettime(CLOCK_MONOTONIC, &end_single);
  double single_ns = (double)(end_single.tv_sec - start_single.tv_sec) * 1e9 +
                     (double)(end_single.tv_nsec - start_single.tv_nsec);
  double c_single_ms = single_ns / (double)ITERS / 1e6;

  // Benchmark Multi
  struct timespec start_multi, end_multi;
  clock_gettime(CLOCK_MONOTONIC, &start_multi);
  for (int i = 0; i < ITERS; i++) {
    pipeline_process(pipeline_multi, input, output);
  }
  clock_gettime(CLOCK_MONOTONIC, &end_multi);
  double multi_ns = (double)(end_multi.tv_sec - start_multi.tv_sec) * 1e9 +
                    (double)(end_multi.tv_nsec - start_multi.tv_nsec);
  double c_multi_ms = multi_ns / (double)ITERS / 1e6;

  // Load Rust results
  pipeline_rust_results_t rust =
      run_upstream_pipeline_benchmark("biquad_single", "biquad_multi");
  double rust_single = rust.single_ms;
  double rust_multi = rust.multi_ms;

  print_comparison_table(
      "Upstream Match: 4-in 2-out Biquad Pipeline (96 EQ evaluations)",
      rust_single, rust_multi, c_single_ms, c_multi_ms);

  audio_chunk_free(input);
  audio_chunk_free(output);
  pipeline_free(pipeline_single);
  pipeline_free(pipeline_multi);
  processing_parameters_free(params);
}

TEST(Pipeline_Biquads_Conv_Benchmark) {
  dsp_config_t config;
  memset(&config, 0, sizeof(dsp_config_t));
  config.devices.samplerate = SAMPLE_RATE;
  config.devices.chunksize = CHUNK_SIZE;
  config.devices.capture.type = AUDIO_BACKEND_TYPE_FILE;
  config.devices.capture.cfg.raw_file.channels = 4;
  config.devices.playback.type = AUDIO_BACKEND_TYPE_FILE;
  config.devices.playback.cfg.raw_file.channels = 2;

  named_filter_config_t filters[36];
  memset(filters, 0, sizeof(filters));

  char* pre_names[18];
  for (int i = 0; i < 16; i++) {
    sprintf(filters[i].name, "pre_bq_%d", i + 1);
    filters[i].filter.type = FILTER_TYPE_BIQUAD;
    filters[i].filter.parameters.biquad.type = BIQUAD_TYPE_PEAKING;
    filters[i].filter.parameters.biquad.freq = pre_bq_freqs[i];
    filters[i].filter.parameters.biquad.q = pre_bq_qs[i];
    filters[i].filter.parameters.biquad.gain = 1.5;
    pre_names[i] = filters[i].name;
  }

  char* post_names[18];
  for (int i = 0; i < 16; i++) {
    sprintf(filters[16 + i].name, "post_bq_%d", i + 1);
    filters[16 + i].filter.type = FILTER_TYPE_BIQUAD;
    filters[16 + i].filter.parameters.biquad.type = BIQUAD_TYPE_PEAKING;
    filters[16 + i].filter.parameters.biquad.freq = post_bq_freqs[i];
    filters[16 + i].filter.parameters.biquad.q = post_bq_qs[i];
    filters[16 + i].filter.parameters.biquad.gain = 1.5;
    post_names[i] = filters[16 + i].name;
  }

  // Pre convs
  double* pre_coeffs1 = build_conv_filter_coefficients(32768);
  strcpy(filters[32].name, "pre_conv_1");
  filters[32].filter.type = FILTER_TYPE_CONV;
  filters[32].filter.parameters.conv.type = CONV_TYPE_VALUES;
  filters[32].filter.parameters.conv.values = pre_coeffs1;
  filters[32].filter.parameters.conv.values_count = 32768;
  pre_names[16] = filters[32].name;

  double* pre_coeffs2 = build_conv_filter_coefficients(65536);
  strcpy(filters[33].name, "pre_conv_2");
  filters[33].filter.type = FILTER_TYPE_CONV;
  filters[33].filter.parameters.conv.type = CONV_TYPE_VALUES;
  filters[33].filter.parameters.conv.values = pre_coeffs2;
  filters[33].filter.parameters.conv.values_count = 65536;
  pre_names[17] = filters[33].name;

  // Post convs
  double* post_coeffs1 = build_conv_filter_coefficients(32768);
  strcpy(filters[34].name, "post_conv_1");
  filters[34].filter.type = FILTER_TYPE_CONV;
  filters[34].filter.parameters.conv.type = CONV_TYPE_VALUES;
  filters[34].filter.parameters.conv.values = post_coeffs1;
  filters[34].filter.parameters.conv.values_count = 32768;
  post_names[16] = filters[34].name;

  double* post_coeffs2 = build_conv_filter_coefficients(65536);
  strcpy(filters[35].name, "post_conv_2");
  filters[35].filter.type = FILTER_TYPE_CONV;
  filters[35].filter.parameters.conv.type = CONV_TYPE_VALUES;
  filters[35].filter.parameters.conv.values = post_coeffs2;
  filters[35].filter.parameters.conv.values_count = 65536;
  post_names[17] = filters[35].name;

  config.filters = filters;
  config.filters_count = 36;

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
  strcpy(mixer_cfg.name, "mix_4_to_2");
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
  steps[0].names = pre_names;
  steps[0].names_count = 18;

  steps[1].type = PIPELINE_STEP_TYPE_MIXER;
  strcpy(steps[1].name, "mix_4_to_2");
  steps[1].has_name = true;

  steps[2].type = PIPELINE_STEP_TYPE_FILTER;
  steps[2].has_channel = false;
  steps[2].names = post_names;
  steps[2].names_count = 18;

  config.pipeline = steps;
  config.pipeline_count = 3;

  processing_parameters_t* params = processing_parameters_create(4, 2);
  ASSERT_TRUE(params != NULL);

  // Measure Single-Threaded C
  config.devices.multithreaded = false;
  config.devices.has_multithreaded = true;
  pipeline_t* pipeline_single = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline_single != NULL);

  // Measure Multi-Threaded C
  config.devices.multithreaded = true;
  config.devices.has_multithreaded = true;
  pipeline_t* pipeline_multi = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline_multi != NULL);

  audio_chunk_t* input = make_dummy_signal(4);
  audio_chunk_t* output = audio_chunk_create(CHUNK_SIZE, 2);

  // Warm-up
  for (int i = 0; i < 20; i++) {
    pipeline_process(pipeline_single, input, output);
    pipeline_process(pipeline_multi, input, output);
  }

  // Benchmark Single (10 per user request)
  struct timespec start_single, end_single;
  clock_gettime(CLOCK_MONOTONIC, &start_single);
  for (int i = 0; i < 10; i++) {
    pipeline_process(pipeline_single, input, output);
  }
  clock_gettime(CLOCK_MONOTONIC, &end_single);
  double single_ns = (double)(end_single.tv_sec - start_single.tv_sec) * 1e9 +
                     (double)(end_single.tv_nsec - start_single.tv_nsec);
  double c_single_ms = single_ns / 10.0 / 1e6;

  // Benchmark Multi (10 per user request)
  struct timespec start_multi, end_multi;
  clock_gettime(CLOCK_MONOTONIC, &start_multi);
  for (int i = 0; i < 10; i++) {
    pipeline_process(pipeline_multi, input, output);
  }
  clock_gettime(CLOCK_MONOTONIC, &end_multi);
  double multi_ns = (double)(end_multi.tv_sec - start_multi.tv_sec) * 1e9 +
                    (double)(end_multi.tv_nsec - start_multi.tv_nsec);
  double c_multi_ms = multi_ns / 10.0 / 1e6;

  // Load Rust results
  pipeline_rust_results_t rust = run_upstream_pipeline_benchmark(
      "biquad_conv_single", "biquad_conv_multi");
  double rust_single = rust.single_ms;
  double rust_multi = rust.multi_ms;

  print_comparison_table(
      "Upstream Match: 4-in 2-out Biquad + Convolution Pipeline (96 EQ + 12 "
      "long convolve)",
      rust_single, rust_multi, c_single_ms, c_multi_ms);

  audio_chunk_free(input);
  audio_chunk_free(output);
  pipeline_free(pipeline_single);
  pipeline_free(pipeline_multi);
  processing_parameters_free(params);
  free(pre_coeffs1);
  free(pre_coeffs2);
  free(post_coeffs1);
  free(post_coeffs2);
}

TEST_MAIN()
