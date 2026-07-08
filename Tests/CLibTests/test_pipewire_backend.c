#if defined(__linux__)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <string.h>

#include "../../Sources/CDSP/Backend/pipewire_backend.h"
#include "test_support.h"

TEST(PipeWirePlaybackBasic) {
  playback_device_config_t play_cfg;
  memset(&play_cfg, 0, sizeof(play_cfg));
  play_cfg.type = AUDIO_BACKEND_TYPE_PIPEWIRE;
  play_cfg.cfg.pipewire.channels = 2;
  snprintf(play_cfg.cfg.pipewire.device, sizeof(play_cfg.cfg.pipewire.device), "default");
  play_cfg.cfg.pipewire.has_device = true;

  backend_error_t err;
  playback_backend_t* playback =
      pipewire_playback_create(&play_cfg, 48000, 1024, NULL, &err);
  if (!playback) {
    printf("PipeWire playback backend creation failed: %s (skipping test)\n",
           err.message);
    return;
  }

  if (!playback_backend_open(playback, &err)) {
    printf("PipeWire playback backend open failed: %s (skipping test)\n",
           err.message);
    playback_backend_free(playback);
    return;
  }

  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  memset(audio_chunk_get_channel(chunk, 0), 0, 1024 * sizeof(double));
  memset(audio_chunk_get_channel(chunk, 1), 0, 1024 * sizeof(double));
  chunk->valid_frames = 1024;

  ASSERT_TRUE(playback_backend_write(playback, chunk, &err));

  size_t lvl = playback_backend_get_buffer_level(playback);
  printf("PipeWire buffer level: %zu frames\n", lvl);

  audio_chunk_free(chunk);
  playback_backend_close(playback);
  playback_backend_free(playback);
}

TEST(PipeWireCaptureBasic) {
  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_PIPEWIRE;
  cap_cfg.cfg.pipewire.channels = 2;
  snprintf(cap_cfg.cfg.pipewire.device, sizeof(cap_cfg.cfg.pipewire.device), "default");
  cap_cfg.cfg.pipewire.has_device = true;

  backend_error_t err;
  capture_backend_t* capture =
      pipewire_capture_create(&cap_cfg, 48000, 1024, NULL, &err);
  if (!capture) {
    printf("PipeWire capture backend creation failed: %s (skipping test)\n",
           err.message);
    return;
  }

  if (!capture_backend_open(capture, &err)) {
    printf("PipeWire capture backend open failed: %s (skipping test)\n",
           err.message);
    capture_backend_free(capture);
    return;
  }

  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  // Use capture_backend_wait with a 100ms timeout to avoid hanging if no audio
  // source is active.
  if (capture_backend_wait(capture, 100)) {
    ASSERT_TRUE(capture_backend_read(capture, 128, chunk, &err));
    ASSERT_EQ(128, chunk->valid_frames);
  } else {
    printf(
        "No capture data received from PipeWire within 100ms (skipping read "
        "test)\n");
  }

  audio_chunk_free(chunk);
  capture_backend_close(capture);
  capture_backend_free(capture);
}

TEST_MAIN()

#else

#include "test_support.h"
TEST(PipeWireSkippedOnNonLinux) {}
TEST_MAIN()

#endif
