#if defined(__linux__)
#define _GNU_SOURCE
#endif
#include <math.h>
#include <stdlib.h>

#include "../../Sources/CDSP/Audio/audio_chunk.h"
#include "../../Sources/CDSP/Audio/audio_history_buffer.h"
#include "../../Sources/CDSP/Audio/spectrum_analyzer.h"
#include "test_support.h"

static audio_chunk_t* sine_chunk(double freq, int samplerate, size_t frames,
                                 size_t start_frame, size_t channels) {
  audio_chunk_t* chunk = audio_chunk_create(frames, channels);
  audio_chunk_set_valid_frames(chunk, frames);
  double dt = 2.0 * M_PI * freq / (double)samplerate;
  for (size_t ch = 0; ch < channels; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < frames; t++) {
      buf[t] = sin(dt * (double)(start_frame + t));
    }
  }
  return chunk;
}

TEST(SineProducesPeakAtCarrier) {
  spectrum_analyzer_t* analyzer = spectrum_analyzer_create();
  audio_history_buffer_t* buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);
  int samplerate = 48000;

  for (size_t i = 0; i < 16; i++) {
    audio_chunk_t* chunk = sine_chunk(1000.0, samplerate, 1024, i * 1024, 2);
    audio_history_buffer_append(buffer, chunk);
    audio_chunk_free(chunk);
  }

  spectrum_result_t result = {0};
  spectrum_status_t status = spectrum_analyzer_compute(
      analyzer, buffer, 0, 20.0, 20000.0, 64, samplerate, &result);

  ASSERT_EQ(SPECTRUM_OK, status);
  ASSERT_EQ(64, result.count);

  size_t nearest_1k = 0;
  double min_diff = 1e9;
  for (size_t i = 0; i < result.count; i++) {
    double diff = fabs((double)result.frequencies[i] - 1000.0);
    if (diff < min_diff) {
      min_diff = diff;
      nearest_1k = i;
    }
  }

  size_t peak_index = 0;
  float max_mag = -200.0f;
  for (size_t i = 0; i < result.count; i++) {
    if (result.magnitudes[i] > max_mag) {
      max_mag = result.magnitudes[i];
      peak_index = i;
    }
  }

  ASSERT_EQ(nearest_1k, peak_index);
  ASSERT_TRUE(result.magnitudes[peak_index] < 1.0f);
  ASSERT_TRUE(result.magnitudes[peak_index] > -10.0f);

  audio_history_buffer_free(buffer);
  spectrum_analyzer_free(analyzer);
}

TEST(EmptyBufferThrows) {
  spectrum_analyzer_t* analyzer = spectrum_analyzer_create();
  audio_history_buffer_t* buffer = audio_history_buffer_create();
  spectrum_result_t result = {0};

  spectrum_status_t status = spectrum_analyzer_compute(
      analyzer, buffer, -1, 20.0, 20000.0, 32, 48000, &result);
  ASSERT_EQ(SPECTRUM_ERROR_EMPTY, status);

  audio_history_buffer_free(buffer);
  spectrum_analyzer_free(analyzer);
}

TEST(ChannelOutOfRangeThrows) {
  spectrum_analyzer_t* analyzer = spectrum_analyzer_create();
  audio_history_buffer_t* buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);

  audio_chunk_t* chunk = sine_chunk(440.0, 48000, 1024, 0, 2);
  audio_history_buffer_append(buffer, chunk);
  audio_chunk_free(chunk);

  spectrum_result_t result = {0};
  spectrum_status_t status = spectrum_analyzer_compute(
      analyzer, buffer, 4, 20.0, 20000.0, 32, 48000, &result);
  ASSERT_EQ(SPECTRUM_ERROR_OUT_OF_RANGE, status);

  audio_history_buffer_free(buffer);
  spectrum_analyzer_free(analyzer);
}

TEST(LogBinFrequenciesAreGeometric) {
  spectrum_analyzer_t* analyzer = spectrum_analyzer_create();
  audio_history_buffer_t* buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 1);

  audio_chunk_t* chunk = audio_chunk_create(4096, 1);
  audio_chunk_set_valid_frames(chunk, 4096);
  audio_history_buffer_append(buffer, chunk);
  audio_chunk_free(chunk);

  spectrum_result_t result = {0};
  spectrum_status_t status = spectrum_analyzer_compute(
      analyzer, buffer, 0, 20.0, 20000.0, 5, 48000, &result);
  ASSERT_EQ(SPECTRUM_OK, status);
  ASSERT_EQ(5, result.count);
  ASSERT_NEAR(20.0f, result.frequencies[0], 1e-3);
  ASSERT_NEAR(20000.0f, result.frequencies[4], 1.0);

  double ratio01 = (double)(result.frequencies[1] / result.frequencies[0]);
  double ratio34 = (double)(result.frequencies[4] / result.frequencies[3]);
  ASSERT_NEAR(ratio01, ratio34, 1e-3);

  audio_history_buffer_free(buffer);
  spectrum_analyzer_free(analyzer);
}

TEST_MAIN()
