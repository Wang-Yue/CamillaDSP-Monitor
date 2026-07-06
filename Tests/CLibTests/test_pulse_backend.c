#if defined(__linux__)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <string.h>

#include "../../Sources/CDSP/Backend/pulse_backend.h"
#include "test_support.h"

TEST(PulsePlaybackBasic) {
  playback_device_config_t play_cfg;
  memset(&play_cfg, 0, sizeof(play_cfg));
  play_cfg.type = AUDIO_BACKEND_TYPE_PULSE_AUDIO;
  play_cfg.channels = 2;
  // use default device
  snprintf(play_cfg.device, sizeof(play_cfg.device), "default");
  play_cfg.has_device = true;

  backend_error_t err;
  playback_backend_t* playback =
      pulse_playback_create(&play_cfg, 48000, 1024, NULL, &err);
  if (!playback) {
    printf("PulseAudio playback backend creation failed: %s (skipping test)\n",
           err.message);
    return;
  }

  if (!playback_backend_open(playback, &err)) {
    printf("PulseAudio playback backend open failed: %s (skipping test)\n",
           err.message);
    playback_backend_free(playback);
    return;
  }

  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  // Write silence
  memset(audio_chunk_get_channel(chunk, 0), 0, 1024 * sizeof(double));
  memset(audio_chunk_get_channel(chunk, 1), 0, 1024 * sizeof(double));
  chunk->valid_frames = 1024;

  ASSERT_TRUE(playback_backend_write(playback, chunk, &err));

  size_t lvl = playback_backend_get_buffer_level(playback);
  printf("PulseAudio buffer level: %zu frames\n", lvl);

  audio_chunk_free(chunk);
  playback_backend_close(playback);
  playback_backend_free(playback);
}

TEST(PulseCaptureBasic) {
  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_PULSE_AUDIO;
  cap_cfg.channels = 2;
  snprintf(cap_cfg.device, sizeof(cap_cfg.device), "default");
  cap_cfg.has_device = true;

  backend_error_t err;
  capture_backend_t* capture =
      pulse_capture_create(&cap_cfg, 48000, 1024, NULL, &err);
  if (!capture) {
    printf("PulseAudio capture backend creation failed: %s (skipping test)\n",
           err.message);
    return;
  }

  if (!capture_backend_open(capture, &err)) {
    printf("PulseAudio capture backend open failed: %s (skipping test)\n",
           err.message);
    capture_backend_free(capture);
    return;
  }

  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  // Pulse Simple API blocks until requested frames are available.
  // To avoid blocking too long in unit tests, we'll read a small chunk.
  ASSERT_TRUE(capture_backend_read(capture, 128, chunk, &err));
  ASSERT_EQ(128, chunk->valid_frames);

  audio_chunk_free(chunk);
  capture_backend_close(capture);
  capture_backend_free(capture);
}

TEST_MAIN()

#else

#include "test_support.h"
TEST(PulseSkippedOnNonLinux) {
  // PulseAudio is Linux-only in our build
}
TEST_MAIN()

#endif
