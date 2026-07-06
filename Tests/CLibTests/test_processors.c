#include <math.h>

#include "../../Sources/CDSP/Audio/audio_chunk.h"
#include "../../Sources/CDSP/Processors/compressor_processor.h"
#include "../../Sources/CDSP/Processors/noise_gate_processor.h"
#include "../../Sources/CDSP/Processors/processor.h"
#include "../../Sources/CDSP/Processors/race_processor.h"
#include "test_support.h"

static bool is_close(double left, double right, double maxdiff) {
  return fabs(left - right) < maxdiff;
}

TEST(compressor_basic_compression) {
  int mon_ch[] = {0};
  int proc_ch[] = {0, 1};
  compressor_parameters_t params = {0};
  params.channels = 2;
  params.monitor_channels = mon_ch;
  params.monitor_channels_count = 1;
  params.process_channels = proc_ch;
  params.process_channels_count = 2;
  params.attack = 0.002;
  params.release = 0.1;
  params.threshold = -6.02;
  params.factor = 2.0;
  params.makeup_gain = 0.0;
  params.has_makeup_gain = true;
  params.soft_clip = false;
  params.has_clip_limit = false;

  compressor_processor_t* comp =
      compressor_processor_create("compressor", &params, 48000, 1000);
  ASSERT_TRUE(comp != NULL);

  audio_chunk_t* chunk = audio_chunk_create(1000, 2);
  ASSERT_TRUE(chunk != NULL);
  double* ch0 = audio_chunk_get_channel(chunk, 0);
  double* ch1 = audio_chunk_get_channel(chunk, 1);
  for (size_t i = 0; i < 1000; i++) {
    ch0[i] = 1.0;
    ch1[i] = 0.5;
  }
  chunk->valid_frames = 1000;

  compressor_processor_process(comp, chunk);

  ASSERT_TRUE(ch0[999] < 0.8);
  ASSERT_TRUE(ch1[999] < 0.4);

  audio_chunk_free(chunk);
  compressor_processor_free(comp);
}

TEST(noisegate_basic_gate) {
  int mon_ch[] = {0};
  int proc_ch[] = {0};
  noise_gate_parameters_t params = {0};
  params.channels = 1;
  params.monitor_channels = mon_ch;
  params.monitor_channels_count = 1;
  params.process_channels = proc_ch;
  params.process_channels_count = 1;
  params.attack = 0.0001;
  params.release = 0.0001;
  params.threshold = -20.0;
  params.attenuation = 40.0;

  noise_gate_processor_t* gate =
      noise_gate_processor_create("noisegate", &params, 48000, 100);
  ASSERT_TRUE(gate != NULL);

  audio_chunk_t* chunk = audio_chunk_create(100, 1);
  ASSERT_TRUE(chunk != NULL);
  double* ch0 = audio_chunk_get_channel(chunk, 0);
  for (size_t i = 0; i < 100; i++) {
    if (i >= 20 && i < 40) {
      ch0[i] = 0.5;
    } else {
      ch0[i] = 0.001;
    }
  }
  chunk->valid_frames = 100;

  noise_gate_processor_process(gate, chunk);

  ASSERT_TRUE(ch0[35] > 0.4);
  ASSERT_TRUE(ch0[60] < 0.00005);

  audio_chunk_free(chunk);
  noise_gate_processor_free(gate);
}

TEST(race_basic) {
  race_parameters_t params = {0};
  params.channels = 2;
  params.channel_a = 0;
  params.channel_b = 1;
  params.delay = 5.0;
  params.subsample_delay = false;
  params.has_subsample_delay = true;
  params.delay_unit = DELAY_UNIT_SAMPLES;
  params.has_delay_unit = true;
  params.attenuation = 6.02;

  race_processor_t* race = race_processor_create("race", &params, 48000);
  ASSERT_TRUE(race != NULL);

  audio_chunk_t* chunk = audio_chunk_create(10, 2);
  ASSERT_TRUE(chunk != NULL);
  double* ch0 = audio_chunk_get_channel(chunk, 0);
  double* ch1 = audio_chunk_get_channel(chunk, 1);
  ch0[0] = 1.0;
  for (size_t i = 1; i < 10; i++) {
    ch0[i] = 0.0;
    ch1[i] = 0.0;
  }
  chunk->valid_frames = 10;

  race_processor_process(race, chunk);

  ASSERT_DOUBLE_EQ(1.0, ch0[0]);
  ASSERT_TRUE(is_close(ch1[5], -0.5, 1e-4));

  audio_chunk_free(chunk);
  race_processor_free(race);
}

TEST_MAIN()
