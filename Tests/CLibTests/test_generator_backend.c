#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include <math.h>
#include <string.h>
#include <time.h>

#include "../../Sources/CDSP/Backend/generator_capture.h"
#include "test_support.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

TEST(GeneratorSineCorrectness) {
  capture_device_config_t config;
  memset(&config, 0, sizeof(config));
  config.type = AUDIO_BACKEND_TYPE_GENERATOR;
  config.channels = 2;
  config.generator.type = SIGNAL_TYPE_SINE;
  config.generator.frequency = 1000.0;
  config.generator.level = 0.0;
  config.has_generator = true;

  backend_error_t err;
  capture_backend_t* backend =
      generator_capture_create(&config, 44100, 1024, NULL, &err);
  ASSERT_TRUE(backend != NULL);

  ASSERT_TRUE(capture_backend_open(backend, &err));

  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  ASSERT_TRUE(capture_backend_read(backend, 1024, chunk, &err));
  ASSERT_EQ(1024, chunk->valid_frames);

  double expected_phase = 0.0;
  double freq_delta = 1000.0 / 44100.0;
  for (size_t f = 0; f < 1024; f++) {
    double expected_val = sin(expected_phase * 2.0 * M_PI);
    ASSERT_NEAR(expected_val, audio_chunk_get_channel(chunk, 0)[f], 1e-6);
    ASSERT_NEAR(expected_val, audio_chunk_get_channel(chunk, 1)[f], 1e-6);
    expected_phase += freq_delta;
    if (expected_phase >= 1.0) expected_phase -= 1.0;
  }

  audio_chunk_free(chunk);
  capture_backend_close(backend);
  capture_backend_free(backend);
}

TEST(GeneratorSquareCorrectness) {
  capture_device_config_t config;
  memset(&config, 0, sizeof(config));
  config.type = AUDIO_BACKEND_TYPE_GENERATOR;
  config.channels = 1;
  config.generator.type = SIGNAL_TYPE_SQUARE;
  config.generator.frequency = 100.0;
  config.generator.level = -6.020599913279624;
  config.has_generator = true;

  backend_error_t err;
  capture_backend_t* backend =
      generator_capture_create(&config, 44100, 1024, NULL, &err);
  ASSERT_TRUE(backend != NULL);

  ASSERT_TRUE(capture_backend_open(backend, &err));

  audio_chunk_t* chunk = audio_chunk_create(1024, 1);
  ASSERT_TRUE(capture_backend_read(backend, 1024, chunk, &err));

  double expected_phase = 0.0;
  double freq_delta = 100.0 / 44100.0;
  for (size_t f = 0; f < 1024; f++) {
    double expected_val =
        (sin(expected_phase * 2.0 * M_PI) >= 0.0 ? 0.5 : -0.5);
    ASSERT_NEAR(expected_val, audio_chunk_get_channel(chunk, 0)[f], 1e-6);
    expected_phase += freq_delta;
    if (expected_phase >= 1.0) expected_phase -= 1.0;
  }

  audio_chunk_free(chunk);
  capture_backend_close(backend);
  capture_backend_free(backend);
}

TEST(GeneratorNoThrottling) {
  capture_device_config_t config;
  memset(&config, 0, sizeof(config));
  config.type = AUDIO_BACKEND_TYPE_GENERATOR;
  config.channels = 1;
  config.generator.type = SIGNAL_TYPE_SINE;
  config.generator.frequency = 1000.0;
  config.generator.level = 0.0;
  config.has_generator = true;

  backend_error_t err;
  capture_backend_t* backend =
      generator_capture_create(&config, 1000, 100, NULL, &err);
  ASSERT_TRUE(backend != NULL);

  ASSERT_TRUE(capture_backend_open(backend, &err));

  audio_chunk_t* chunk = audio_chunk_create(100, 1);

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);

  for (int i = 0; i < 3; i++) {
    ASSERT_TRUE(capture_backend_read(backend, 100, chunk, &err));
  }

  clock_gettime(CLOCK_MONOTONIC, &end);
  double elapsed_ms = (double)(end.tv_sec - start.tv_sec) * 1000.0 +
                      (double)(end.tv_nsec - start.tv_nsec) / 1000000.0;

  ASSERT_TRUE(elapsed_ms < 10.0);

  audio_chunk_free(chunk);
  capture_backend_close(backend);
  capture_backend_free(backend);
}

TEST_MAIN()
