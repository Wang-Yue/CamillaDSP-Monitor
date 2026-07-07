#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../Sources/CDSP/Backend/file_backend.h"
#include "test_support.h"

TEST(FileBackendRawRoundTrip) {
  const char* raw_filename = "/tmp/test_file_backend_roundtrip.raw";
  remove(raw_filename);

  // 1. Write raw float values to file
  playback_device_config_t play_cfg;
  memset(&play_cfg, 0, sizeof(play_cfg));
  play_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  play_cfg.channels = 2;
  snprintf(play_cfg.filename, sizeof(play_cfg.filename), "%s", raw_filename);
  play_cfg.file_format = BINARY_SAMPLE_FORMAT_F32_LE;
  play_cfg.is_wav = false;
  play_cfg.has_filename = true;
  play_cfg.has_file_format = true;
  play_cfg.has_is_wav = true;

  backend_error_t err;
  playback_backend_t* playback =
      file_playback_create(&play_cfg, 44100, 1024, NULL, &err);
  ASSERT_TRUE(playback != NULL);
  ASSERT_TRUE(playback_backend_open(playback, &err));

  audio_chunk_t* write_chunk = audio_chunk_create(100, 2);
  for (size_t f = 0; f < 100; f++) {
    audio_chunk_get_channel(write_chunk, 0)[f] = (double)f / 100.0;
    audio_chunk_get_channel(write_chunk, 1)[f] = -(double)f / 100.0;
  }
  write_chunk->valid_frames = 100;

  ASSERT_TRUE(playback_backend_write(playback, write_chunk, &err));
  playback_backend_close(playback);
  playback_backend_free(playback);

  // 2. Read back and verify
  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  cap_cfg.channels = 2;
  snprintf(cap_cfg.filename, sizeof(cap_cfg.filename), "%s", raw_filename);
  cap_cfg.file_format = BINARY_SAMPLE_FORMAT_F32_LE;
  cap_cfg.is_wav = false;
  cap_cfg.has_filename = true;
  cap_cfg.has_file_format = true;
  cap_cfg.has_is_wav = true;

  capture_backend_t* capture =
      file_capture_create(&cap_cfg, 44100, 1024, NULL, &err);
  ASSERT_TRUE(capture != NULL);
  ASSERT_TRUE(capture_backend_open(capture, &err));

  audio_chunk_t* read_chunk = audio_chunk_create(100, 2);
  ASSERT_TRUE(capture_backend_read(capture, 100, read_chunk, &err));
  ASSERT_EQ(100, read_chunk->valid_frames);

  for (size_t f = 0; f < 100; f++) {
    ASSERT_NEAR((double)f / 100.0, audio_chunk_get_channel(read_chunk, 0)[f],
                1e-6);
    ASSERT_NEAR(-(double)f / 100.0, audio_chunk_get_channel(read_chunk, 1)[f],
                1e-6);
  }

  audio_chunk_free(write_chunk);
  audio_chunk_free(read_chunk);
  capture_backend_close(capture);
  capture_backend_free(capture);

  remove(raw_filename);
}

TEST(FileBackendWavRoundTrip) {
  const char* wav_filename = "/tmp/test_file_backend_roundtrip.wav";
  remove(wav_filename);

  // 1. Write WAV file (S16 format)
  playback_device_config_t play_cfg;
  memset(&play_cfg, 0, sizeof(play_cfg));
  play_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  play_cfg.channels = 1;
  snprintf(play_cfg.filename, sizeof(play_cfg.filename), "%s", wav_filename);
  play_cfg.file_format = BINARY_SAMPLE_FORMAT_S16_LE;
  play_cfg.is_wav = true;  // Request WAV header!
  play_cfg.has_filename = true;
  play_cfg.has_file_format = true;
  play_cfg.has_is_wav = true;

  backend_error_t err;
  playback_backend_t* playback =
      file_playback_create(&play_cfg, 16000, 1024, NULL, &err);
  ASSERT_TRUE(playback != NULL);
  ASSERT_TRUE(playback_backend_open(playback, &err));

  audio_chunk_t* write_chunk = audio_chunk_create(128, 1);
  for (size_t f = 0; f < 128; f++) {
    // Values in [-1.0, 1.0]. S16 will quantize them.
    audio_chunk_get_channel(write_chunk, 0)[f] = (double)f / 128.0;
  }
  write_chunk->valid_frames = 128;

  ASSERT_TRUE(playback_backend_write(playback, write_chunk, &err));
  playback_backend_close(playback);
  playback_backend_free(playback);

  // 2. Read back using WavFile type (checks headers dynamically!)
  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  cap_cfg.channels = 1;
  snprintf(cap_cfg.filename, sizeof(cap_cfg.filename), "%s", wav_filename);
  cap_cfg.is_wav = true;
  cap_cfg.has_filename = true;
  cap_cfg.has_is_wav = true;

  // Notice we pass sample_rate = 0, channels = 0 to verify that the
  // open routine updates them from the WAV header!
  capture_backend_t* capture =
      file_capture_create(&cap_cfg, 0, 1024, NULL, &err);
  ASSERT_TRUE(capture != NULL);
  ASSERT_TRUE(capture_backend_open(capture, &err));

  audio_chunk_t* read_chunk = audio_chunk_create(128, 1);
  ASSERT_TRUE(capture_backend_read(capture, 128, read_chunk, &err));
  ASSERT_EQ(128, read_chunk->valid_frames);

  for (size_t f = 0; f < 128; f++) {
    double expected = (double)f / 128.0;
    // Quantization error for S16 is +/- 1/32768
    ASSERT_NEAR(expected, audio_chunk_get_channel(read_chunk, 0)[f],
                1.0 / 32767.0);
  }

  audio_chunk_free(write_chunk);
  audio_chunk_free(read_chunk);
  capture_backend_close(capture);
  capture_backend_free(capture);

  remove(wav_filename);
}

TEST(FileBackendPauseThrottling) {
  const char* raw_filename = "/tmp/test_file_backend_pause.raw";
  remove(raw_filename);

  // 1. Write 200 frames to temp file
  playback_device_config_t play_cfg;
  memset(&play_cfg, 0, sizeof(play_cfg));
  play_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  play_cfg.channels = 2;
  snprintf(play_cfg.filename, sizeof(play_cfg.filename), "%s", raw_filename);
  play_cfg.file_format = BINARY_SAMPLE_FORMAT_F32_LE;
  play_cfg.is_wav = false;
  play_cfg.has_filename = true;
  play_cfg.has_file_format = true;
  play_cfg.has_is_wav = true;

  backend_error_t err;
  playback_backend_t* playback =
      file_playback_create(&play_cfg, 44100, 1024, NULL, &err);
  ASSERT_TRUE(playback != NULL);
  ASSERT_TRUE(playback_backend_open(playback, &err));

  audio_chunk_t* write_chunk = audio_chunk_create(200, 2);
  for (size_t f = 0; f < 200; f++) {
    audio_chunk_get_channel(write_chunk, 0)[f] = (double)f / 200.0;
    audio_chunk_get_channel(write_chunk, 1)[f] = -(double)f / 200.0;
  }
  write_chunk->valid_frames = 200;

  ASSERT_TRUE(playback_backend_write(playback, write_chunk, &err));
  playback_backend_close(playback);
  playback_backend_free(playback);

  // 2. Open capture
  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  cap_cfg.channels = 2;
  snprintf(cap_cfg.filename, sizeof(cap_cfg.filename), "%s", raw_filename);
  cap_cfg.file_format = BINARY_SAMPLE_FORMAT_F32_LE;
  cap_cfg.is_wav = false;
  cap_cfg.has_filename = true;
  cap_cfg.has_file_format = true;
  cap_cfg.has_is_wav = true;

  capture_backend_t* capture =
      file_capture_create(&cap_cfg, 44100, 1024, NULL, &err);
  ASSERT_TRUE(capture != NULL);
  ASSERT_TRUE(capture_backend_open(capture, &err));

  // 3. Read first 50 frames
  audio_chunk_t* read_chunk = audio_chunk_create(50, 2);
  ASSERT_TRUE(capture_backend_read(capture, 50, read_chunk, &err));
  ASSERT_EQ(50, read_chunk->valid_frames);
  for (size_t f = 0; f < 50; f++) {
    ASSERT_NEAR((double)f / 200.0, audio_chunk_get_channel(read_chunk, 0)[f], 1e-6);
  }

  // 4. Pause capture and verify read returns false with 0 valid frames
  capture_backend_set_is_paused(capture, true);
  ASSERT_FALSE(capture_backend_read(capture, 50, read_chunk, &err));
  ASSERT_EQ(0, read_chunk->valid_frames);

  // 5. Unpause capture and read next 50 frames (should be frames 50..99)
  capture_backend_set_is_paused(capture, false);
  ASSERT_TRUE(capture_backend_read(capture, 50, read_chunk, &err));
  ASSERT_EQ(50, read_chunk->valid_frames);
  for (size_t f = 0; f < 50; f++) {
    ASSERT_NEAR((double)(f + 50) / 200.0, audio_chunk_get_channel(read_chunk, 0)[f], 1e-6);
  }

  audio_chunk_free(write_chunk);
  audio_chunk_free(read_chunk);
  capture_backend_close(capture);
  capture_backend_free(capture);

  remove(raw_filename);
}

static void run_format_roundtrip_test(binary_sample_format_t format, double eps) {
  const char* raw_filename = "/tmp/test_file_backend_roundtrip_tmp.raw";
  remove(raw_filename);

  playback_device_config_t play_cfg;
  memset(&play_cfg, 0, sizeof(play_cfg));
  play_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  play_cfg.channels = 2;
  snprintf(play_cfg.filename, sizeof(play_cfg.filename), "%s", raw_filename);
  play_cfg.file_format = format;
  play_cfg.is_wav = false;
  play_cfg.has_filename = true;
  play_cfg.has_file_format = true;
  play_cfg.has_is_wav = true;

  backend_error_t err;
  playback_backend_t* playback =
      file_playback_create(&play_cfg, 44100, 64, NULL, &err);
  ASSERT_TRUE(playback != NULL);
  ASSERT_TRUE(playback_backend_open(playback, &err));

  audio_chunk_t* write_chunk = audio_chunk_create(10, 2);
  for (size_t f = 0; f < 10; f++) {
    audio_chunk_get_channel(write_chunk, 0)[f] = (double)f / 10.0;
    audio_chunk_get_channel(write_chunk, 1)[f] = -(double)f / 10.0;
  }
  write_chunk->valid_frames = 10;

  ASSERT_TRUE(playback_backend_write(playback, write_chunk, &err));
  playback_backend_close(playback);
  playback_backend_free(playback);

  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  cap_cfg.channels = 2;
  snprintf(cap_cfg.filename, sizeof(cap_cfg.filename), "%s", raw_filename);
  cap_cfg.file_format = format;
  cap_cfg.is_wav = false;
  cap_cfg.has_filename = true;
  cap_cfg.has_file_format = true;
  cap_cfg.has_is_wav = true;

  capture_backend_t* capture =
      file_capture_create(&cap_cfg, 44100, 64, NULL, &err);
  ASSERT_TRUE(capture != NULL);
  ASSERT_TRUE(capture_backend_open(capture, &err));

  audio_chunk_t* read_chunk = audio_chunk_create(10, 2);
  ASSERT_TRUE(capture_backend_read(capture, 10, read_chunk, &err));
  ASSERT_EQ(10, read_chunk->valid_frames);

  for (size_t f = 0; f < 10; f++) {
    ASSERT_NEAR((double)f / 10.0, audio_chunk_get_channel(read_chunk, 0)[f], eps);
    ASSERT_NEAR(-(double)f / 10.0, audio_chunk_get_channel(read_chunk, 1)[f], eps);
  }

  audio_chunk_free(write_chunk);
  audio_chunk_free(read_chunk);
  capture_backend_close(capture);
  capture_backend_free(capture);

  remove(raw_filename);
}

TEST(FileBackendS16RoundTrip) {
  run_format_roundtrip_test(BINARY_SAMPLE_FORMAT_S16_LE, 1e-4);
}

TEST(FileBackendS24_3RoundTrip) {
  run_format_roundtrip_test(BINARY_SAMPLE_FORMAT_S24_3_LE, 1e-6);
}

TEST(FileBackendS24_4RJRoundTrip) {
  run_format_roundtrip_test(BINARY_SAMPLE_FORMAT_S24_4_RJ_LE, 1e-6);
}

TEST(FileBackendS24_4LJRoundTrip) {
  run_format_roundtrip_test(BINARY_SAMPLE_FORMAT_S24_4_LJ_LE, 1e-6);
}

TEST(FileBackendS32RoundTrip) {
  run_format_roundtrip_test(BINARY_SAMPLE_FORMAT_S32_LE, 1e-9);
}

TEST(FileBackendF32RoundTrip) {
  run_format_roundtrip_test(BINARY_SAMPLE_FORMAT_F32_LE, 1e-6);
}

TEST(FileBackendF64RoundTrip) {
  run_format_roundtrip_test(BINARY_SAMPLE_FORMAT_F64_LE, 1e-12);
}

TEST_MAIN()
