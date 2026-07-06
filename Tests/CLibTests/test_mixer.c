#include <math.h>
#include <stdlib.h>

#include "../../Sources/CDSP/Mixer/mixer.h"
#include "test_support.h"

static audio_chunk_t* make_constant_chunk(size_t frames, size_t channels,
                                          double value) {
  audio_chunk_t* chunk = audio_chunk_create(frames, channels);
  for (size_t ch = 0; ch < channels; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(chunk, ch);
    for (size_t i = 0; i < frames; i++) {
      buf[i] = value;
    }
  }
  return chunk;
}

static void assert_all_samples_ch(const audio_chunk_t* chunk, size_t ch,
                                  double expected, double accuracy) {
  waveform_t buf = audio_chunk_get_channel(chunk, ch);
  for (size_t i = 0; i < chunk->valid_frames; i++) {
    double diff = fabs(buf[i] - expected);
    if (diff > accuracy) {
      printf(
          "Sample mismatch at ch %zu, idx %zu: expected %g, got %g (diff %g)\n",
          ch, i, expected, buf[i], diff);
    }
    ASSERT_NEAR(expected, buf[i], accuracy);
  }
}

static void assert_silence_ch(const audio_chunk_t* chunk, size_t ch) {
  assert_all_samples_ch(chunk, ch, 0.0, 1e-9);
}

TEST(MixerConstruction2to4) {
  mixer_source_t src0 = {
      .channel = 0, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB};
  mixer_source_t src1 = {
      .channel = 1, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB};
  mixer_mapping_t maps[4] = {{.dest = 0, .sources_count = 1, .sources = &src0},
                             {.dest = 1, .sources_count = 1, .sources = &src1},
                             {.dest = 2, .sources_count = 1, .sources = &src0},
                             {.dest = 3, .sources_count = 1, .sources = &src1}};
  mixer_config_t config = {
      .channels_in = 2, .channels_out = 4, .mapping_count = 4, .mapping = maps};
  audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);
  ASSERT_TRUE(mixer != NULL);
  ASSERT_EQ(2, mixer->channels_in);
  ASSERT_EQ(4, mixer->channels_out);

  audio_chunk_t* input = make_constant_chunk(8, 2, 1.0);
  audio_chunk_t* output = audio_mixer_process_chunk(mixer, input);
  ASSERT_TRUE(output != NULL);
  ASSERT_EQ(4, audio_chunk_get_channels(output));
  ASSERT_EQ(input->valid_frames, output->valid_frames);

  double expected_linear = double_from_db(0.0);
  for (size_t ch = 0; ch < 4; ch++) {
    assert_all_samples_ch(output, ch, expected_linear, 1e-9);
  }
  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_mixer_free(mixer);
}

TEST(MixerMutedMapping) {
  mixer_source_t src0 = {
      .channel = 0, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB};
  mixer_source_t src1 = {
      .channel = 1, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB};
  mixer_mapping_t maps[4] = {
      {.dest = 0, .sources_count = 1, .sources = &src0, .mute = true},
      {.dest = 1, .sources_count = 1, .sources = &src1},
      {.dest = 2, .sources_count = 1, .sources = &src0, .mute = true},
      {.dest = 3, .sources_count = 1, .sources = &src1}};
  mixer_config_t config = {
      .channels_in = 2, .channels_out = 4, .mapping_count = 4, .mapping = maps};
  audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);
  audio_chunk_t* input = make_constant_chunk(8, 2, 1.0);
  audio_chunk_t* output = audio_mixer_process_chunk(mixer, input);

  ASSERT_EQ(4, audio_chunk_get_channels(output));
  assert_silence_ch(output, 0);
  assert_silence_ch(output, 2);
  assert_all_samples_ch(output, 1, 1.0, 1e-9);
  assert_all_samples_ch(output, 3, 1.0, 1e-9);

  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_mixer_free(mixer);
}

TEST(MixerMutedSource) {
  mixer_source_t srcs[2] = {
      {.channel = 0, .gain = 0.0, .has_gain = true, .mute = true},
      {.channel = 1, .gain = 0.0, .has_gain = true}};
  mixer_mapping_t map = {.dest = 0, .sources_count = 2, .sources = srcs};
  mixer_config_t config = {
      .channels_in = 2, .channels_out = 1, .mapping_count = 1, .mapping = &map};
  audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);

  audio_chunk_t* input = audio_chunk_create(4, 2);
  mutable_waveform_t b0 = audio_chunk_get_channel(input, 0);
  mutable_waveform_t b1 = audio_chunk_get_channel(input, 1);
  for (int i = 0; i < 4; i++) {
    b0[i] = 1.0;
    b1[i] = 0.5;
  }

  audio_chunk_t* output = audio_mixer_process_chunk(mixer, input);
  ASSERT_EQ(1, audio_chunk_get_channels(output));
  assert_all_samples_ch(output, 0, 0.5, 1e-9);

  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_mixer_free(mixer);
}

TEST(MixerStereoToMono) {
  mixer_source_t srcs[2] = {{.channel = 0, .gain = -6.0, .has_gain = true},
                            {.channel = 1, .gain = -6.0, .has_gain = true}};
  mixer_mapping_t map = {.dest = 0, .sources_count = 2, .sources = srcs};
  mixer_config_t config = {
      .channels_in = 2, .channels_out = 1, .mapping_count = 1, .mapping = &map};
  audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);

  audio_chunk_t* input = make_constant_chunk(4, 2, 1.0);
  audio_chunk_t* output = audio_mixer_process_chunk(mixer, input);
  ASSERT_EQ(1, audio_chunk_get_channels(output));
  ASSERT_EQ(4, output->valid_frames);

  double expected = double_from_db(-6.0) * 2.0;
  assert_all_samples_ch(output, 0, expected, 1e-6);

  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_mixer_free(mixer);
}

TEST(MixerMonoToStereo) {
  mixer_source_t src0 = {.channel = 0, .gain = 0.0, .has_gain = true};
  mixer_mapping_t maps[2] = {{.dest = 0, .sources_count = 1, .sources = &src0},
                             {.dest = 1, .sources_count = 1, .sources = &src0}};
  mixer_config_t config = {
      .channels_in = 1, .channels_out = 2, .mapping_count = 2, .mapping = maps};
  audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);

  audio_chunk_t* input = audio_chunk_create(4, 1);
  double samples[] = {0.25, -0.5, 0.75, -1.0};
  mutable_waveform_t b0 = audio_chunk_get_channel(input, 0);
  for (int i = 0; i < 4; i++) b0[i] = samples[i];

  audio_chunk_t* output = audio_mixer_process_chunk(mixer, input);
  ASSERT_EQ(2, audio_chunk_get_channels(output));
  ASSERT_EQ(4, output->valid_frames);

  waveform_t o0 = audio_chunk_get_channel(output, 0);
  waveform_t o1 = audio_chunk_get_channel(output, 1);
  for (int i = 0; i < 4; i++) {
    ASSERT_NEAR(samples[i], o0[i], 1e-9);
    ASSERT_NEAR(samples[i], o1[i], 1e-9);
  }
  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_mixer_free(mixer);
}

TEST(Mixer4to2Downmix) {
  mixer_source_t srcs0[2] = {{.channel = 0, .gain = 0.0, .has_gain = true},
                             {.channel = 2, .gain = -6.0, .has_gain = true}};
  mixer_source_t srcs1[2] = {{.channel = 1, .gain = 0.0, .has_gain = true},
                             {.channel = 3, .gain = -6.0, .has_gain = true}};
  mixer_mapping_t maps[2] = {{.dest = 0, .sources_count = 2, .sources = srcs0},
                             {.dest = 1, .sources_count = 2, .sources = srcs1}};
  mixer_config_t config = {
      .channels_in = 4, .channels_out = 2, .mapping_count = 2, .mapping = maps};
  audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);
  audio_chunk_t* input = make_constant_chunk(8, 4, 1.0);
  audio_chunk_t* output = audio_mixer_process_chunk(mixer, input);

  double expected = 1.0 + double_from_db(-6.0);
  assert_all_samples_ch(output, 0, expected, 1e-6);
  assert_all_samples_ch(output, 1, expected, 1e-6);

  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_mixer_free(mixer);
}

TEST(MixerWithInvertedSource) {
  mixer_source_t srcs[2] = {
      {.channel = 0, .gain = 0.0, .has_gain = true},
      {.channel = 0, .gain = 0.0, .has_gain = true, .inverted = true}};
  mixer_mapping_t map = {.dest = 0, .sources_count = 2, .sources = srcs};
  mixer_config_t config = {
      .channels_in = 1, .channels_out = 1, .mapping_count = 1, .mapping = &map};
  audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);

  audio_chunk_t* input = audio_chunk_create(4, 1);
  double samples[] = {1.0, -0.5, 0.25, 0.8};
  mutable_waveform_t b0 = audio_chunk_get_channel(input, 0);
  for (int i = 0; i < 4; i++) b0[i] = samples[i];

  audio_chunk_t* output = audio_mixer_process_chunk(mixer, input);
  assert_silence_ch(output, 0);

  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_mixer_free(mixer);
}

TEST(MixerIdentity) {
  mixer_source_t src0 = {.channel = 0, .gain = 0.0, .has_gain = true};
  mixer_source_t src1 = {.channel = 1, .gain = 0.0, .has_gain = true};
  mixer_mapping_t maps[2] = {{.dest = 0, .sources_count = 1, .sources = &src0},
                             {.dest = 1, .sources_count = 1, .sources = &src1}};
  mixer_config_t config = {
      .channels_in = 2, .channels_out = 2, .mapping_count = 2, .mapping = maps};
  audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);

  audio_chunk_t* input = audio_chunk_create(4, 2);
  double ch0[] = {0.1, -0.2, 0.3, -0.4};
  double ch1[] = {0.5, -0.6, 0.7, -0.8};
  mutable_waveform_t b0 = audio_chunk_get_channel(input, 0);
  mutable_waveform_t b1 = audio_chunk_get_channel(input, 1);
  for (int i = 0; i < 4; i++) {
    b0[i] = ch0[i];
    b1[i] = ch1[i];
  }

  audio_chunk_t* output = audio_mixer_process_chunk(mixer, input);
  waveform_t o0 = audio_chunk_get_channel(output, 0);
  waveform_t o1 = audio_chunk_get_channel(output, 1);
  for (int i = 0; i < 4; i++) {
    ASSERT_NEAR(ch0[i], o0[i], 1e-9);
    ASSERT_NEAR(ch1[i], o1[i], 1e-9);
  }
  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_mixer_free(mixer);
}

TEST(MixerChannelRouting) {
  mixer_source_t src1 = {.channel = 1, .gain = 0.0, .has_gain = true};
  mixer_source_t src0 = {.channel = 0, .gain = 0.0, .has_gain = true};
  mixer_mapping_t maps[2] = {{.dest = 0, .sources_count = 1, .sources = &src1},
                             {.dest = 1, .sources_count = 1, .sources = &src0}};
  mixer_config_t config = {
      .channels_in = 2, .channels_out = 2, .mapping_count = 2, .mapping = maps};
  audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);

  audio_chunk_t* input = audio_chunk_create(4, 2);
  double ch0[] = {1.0, 2.0, 3.0, 4.0};
  double ch1[] = {-1.0, -2.0, -3.0, -4.0};
  mutable_waveform_t b0 = audio_chunk_get_channel(input, 0);
  mutable_waveform_t b1 = audio_chunk_get_channel(input, 1);
  for (int i = 0; i < 4; i++) {
    b0[i] = ch0[i];
    b1[i] = ch1[i];
  }

  audio_chunk_t* output = audio_mixer_process_chunk(mixer, input);
  waveform_t o0 = audio_chunk_get_channel(output, 0);
  waveform_t o1 = audio_chunk_get_channel(output, 1);
  for (int i = 0; i < 4; i++) {
    ASSERT_NEAR(ch1[i], o0[i], 1e-9);
    ASSERT_NEAR(ch0[i], o1[i], 1e-9);
  }
  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_mixer_free(mixer);
}

TEST(MixerGainAccuracy) {
  mixer_source_t src0 = {.channel = 0, .gain = 6.0, .has_gain = true};
  mixer_source_t src1 = {.channel = 0, .gain = -6.0, .has_gain = true};
  mixer_source_t src2 = {
      .channel = 0, .gain = 0.0, .has_gain = true, .mute = true};
  mixer_mapping_t maps[3] = {{.dest = 0, .sources_count = 1, .sources = &src0},
                             {.dest = 1, .sources_count = 1, .sources = &src1},
                             {.dest = 2, .sources_count = 1, .sources = &src2}};
  mixer_config_t config = {
      .channels_in = 1, .channels_out = 3, .mapping_count = 3, .mapping = maps};
  audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);

  audio_chunk_t* input = make_constant_chunk(4, 1, 1.0);
  audio_chunk_t* output = audio_mixer_process_chunk(mixer, input);

  double gain_plus_6 = double_from_db(6.0);
  ASSERT_TRUE(fabs(gain_plus_6 - 2.0) <= 0.01);
  assert_all_samples_ch(output, 0, gain_plus_6, 1e-9);

  double gain_minus_6 = double_from_db(-6.0);
  ASSERT_TRUE(fabs(gain_minus_6 - 0.5) <= 1e-2);
  assert_all_samples_ch(output, 1, gain_minus_6, 1e-9);

  assert_silence_ch(output, 2);

  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_mixer_free(mixer);
}

TEST(MixerWithLinearScale) {
  mixer_source_t src0_0 = {
      .channel = 0, .gain = 0.5, .has_gain = true, .scale = GAIN_SCALE_LINEAR};
  mixer_source_t src0_1 = {
      .channel = 0, .gain = 2.0, .has_gain = true, .scale = GAIN_SCALE_LINEAR};
  mixer_source_t srcs2[2] = {
      {.channel = 0, .gain = 0.5, .has_gain = true, .scale = GAIN_SCALE_LINEAR},
      {.channel = 1,
       .gain = 0.5,
       .has_gain = true,
       .scale = GAIN_SCALE_LINEAR}};
  mixer_mapping_t maps[3] = {
      {.dest = 0, .sources_count = 1, .sources = &src0_0},
      {.dest = 1, .sources_count = 1, .sources = &src0_1},
      {.dest = 2, .sources_count = 2, .sources = srcs2}};
  mixer_config_t config = {
      .channels_in = 2, .channels_out = 3, .mapping_count = 3, .mapping = maps};
  audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);

  audio_chunk_t* input = make_constant_chunk(4, 2, 1.0);
  audio_chunk_t* output = audio_mixer_process_chunk(mixer, input);

  assert_all_samples_ch(output, 0, 0.5, 1e-9);
  assert_all_samples_ch(output, 1, 2.0, 1e-9);
  assert_all_samples_ch(output, 2, 1.0, 1e-9);

  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_mixer_free(mixer);
}

TEST(CheckMakeMixer) {
  mixer_source_t src0 = {.channel = 0, .gain = 0.0, .has_gain = true};
  mixer_source_t src1 = {.channel = 1, .gain = 0.0, .has_gain = true};
  mixer_mapping_t maps[4] = {{.dest = 0, .sources_count = 1, .sources = &src0},
                             {.dest = 1, .sources_count = 1, .sources = &src1},
                             {.dest = 2, .sources_count = 1, .sources = &src0},
                             {.dest = 3, .sources_count = 1, .sources = &src1}};
  mixer_config_t config = {
      .channels_in = 2, .channels_out = 4, .mapping_count = 4, .mapping = maps};
  audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);
  ASSERT_EQ(2, mixer->channels_in);
  ASSERT_EQ(4, mixer->channels_out);

  audio_chunk_t* input = audio_chunk_create(4, 2);
  mutable_waveform_t b0 = audio_chunk_get_channel(input, 0);
  mutable_waveform_t b1 = audio_chunk_get_channel(input, 1);
  b0[0] = 1.0;
  b0[1] = 0.0;
  b0[2] = 0.0;
  b0[3] = 0.0;
  b1[0] = 0.0;
  b1[1] = 1.0;
  b1[2] = 0.0;
  b1[3] = 0.0;

  audio_chunk_t* output = audio_mixer_process_chunk(mixer, input);
  ASSERT_EQ(4, audio_chunk_get_channels(output));
  ASSERT_EQ(4, output->valid_frames);

  waveform_t o0 = audio_chunk_get_channel(output, 0);
  waveform_t o1 = audio_chunk_get_channel(output, 1);
  waveform_t o2 = audio_chunk_get_channel(output, 2);
  waveform_t o3 = audio_chunk_get_channel(output, 3);

  ASSERT_NEAR(1.0, o0[0], 1e-9);
  ASSERT_NEAR(0.0, o0[1], 1e-9);
  ASSERT_NEAR(0.0, o1[0], 1e-9);
  ASSERT_NEAR(1.0, o1[1], 1e-9);
  ASSERT_NEAR(1.0, o2[0], 1e-9);
  ASSERT_NEAR(0.0, o2[1], 1e-9);
  ASSERT_NEAR(0.0, o3[0], 1e-9);
  ASSERT_NEAR(1.0, o3[1], 1e-9);

  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_mixer_free(mixer);
}

TEST(CheckMakeMixerMuted) {
  mixer_source_t src0 = {.channel = 0, .gain = 0.0, .has_gain = true};
  mixer_source_t src1 = {.channel = 1, .gain = 0.0, .has_gain = true};
  mixer_mapping_t maps[4] = {
      {.dest = 0, .sources_count = 1, .sources = &src0, .mute = true},
      {.dest = 1, .sources_count = 1, .sources = &src1},
      {.dest = 2, .sources_count = 1, .sources = &src0, .mute = true},
      {.dest = 3, .sources_count = 1, .sources = &src1}};
  mixer_config_t config = {
      .channels_in = 2, .channels_out = 4, .mapping_count = 4, .mapping = maps};
  audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);
  audio_chunk_t* input = audio_chunk_create(4, 2);
  mutable_waveform_t b0 = audio_chunk_get_channel(input, 0);
  mutable_waveform_t b1 = audio_chunk_get_channel(input, 1);
  b0[0] = 1.0;
  b1[1] = 1.0;

  audio_chunk_t* output = audio_mixer_process_chunk(mixer, input);
  assert_silence_ch(output, 0);
  assert_silence_ch(output, 2);
  waveform_t o1 = audio_chunk_get_channel(output, 1);
  waveform_t o3 = audio_chunk_get_channel(output, 3);
  ASSERT_NEAR(0.0, o1[0], 1e-9);
  ASSERT_NEAR(1.0, o1[1], 1e-9);
  ASSERT_NEAR(0.0, o3[0], 1e-9);
  ASSERT_NEAR(1.0, o3[1], 1e-9);

  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_mixer_free(mixer);
}

TEST(MixerValidFramesPropagation) {
  mixer_source_t src0 = {.channel = 0, .gain = 0.0, .has_gain = true};
  mixer_mapping_t map = {.dest = 0, .sources_count = 1, .sources = &src0};
  mixer_config_t config = {
      .channels_in = 1, .channels_out = 1, .mapping_count = 1, .mapping = &map};
  audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);

  audio_chunk_t* input = audio_chunk_create(16, 1);
  input->valid_frames = 10;
  audio_chunk_t* output = audio_mixer_process_chunk(mixer, input);
  ASSERT_EQ(10, output->valid_frames);

  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_mixer_free(mixer);
}

TEST(MixerUnmappedOutputIsSilent) {
  mixer_source_t src0 = {.channel = 0, .gain = 0.0, .has_gain = true};
  mixer_mapping_t map = {.dest = 0, .sources_count = 1, .sources = &src0};
  mixer_config_t config = {
      .channels_in = 1, .channels_out = 2, .mapping_count = 1, .mapping = &map};
  audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);

  audio_chunk_t* input = make_constant_chunk(4, 1, 1.0);
  audio_chunk_t* output = audio_mixer_process_chunk(mixer, input);
  assert_all_samples_ch(output, 0, 1.0, 1e-9);
  assert_silence_ch(output, 1);

  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_mixer_free(mixer);
}

TEST(MixerInoutAPI_MatchesAllocatingAPI) {
  mixer_source_t srcs0[2] = {{.channel = 0, .gain = -3.0, .has_gain = true},
                             {.channel = 1, .gain = -6.0, .has_gain = true}};
  mixer_source_t src1 = {.channel = 1, .gain = 0.0, .has_gain = true};
  mixer_source_t src2 = {
      .channel = 0, .gain = 0.0, .has_gain = true, .mute = true};
  mixer_mapping_t maps[3] = {
      {.dest = 0, .sources_count = 2, .sources = srcs0},
      {.dest = 1, .sources_count = 1, .sources = &src1},
      {.dest = 2, .sources_count = 1, .sources = &src2, .mute = true}};
  mixer_config_t config = {
      .channels_in = 2, .channels_out = 3, .mapping_count = 3, .mapping = maps};
  audio_mixer_t* mixerA = audio_mixer_create("mixer", &config, 2048);
  audio_mixer_t* mixerB = audio_mixer_create("mixer", &config, 2048);

  audio_chunk_t* input = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(input, ch);
    for (size_t i = 0; i < 1024; i++) {
      buf[i] = (double)(i % 100) * 0.01 - 0.5;
    }
  }

  audio_chunk_t* out_alloc = audio_mixer_process_chunk(mixerA, input);
  audio_chunk_t* preallocated = audio_chunk_create(1024, 3);
  preallocated->valid_frames = 0;
  mixer_error_t err = audio_mixer_process(mixerB, input, preallocated);
  ASSERT_EQ(MIXER_OK, err);
  ASSERT_EQ(out_alloc->valid_frames, preallocated->valid_frames);

  for (size_t ch = 0; ch < 3; ch++) {
    waveform_t oA = audio_chunk_get_channel(out_alloc, ch);
    waveform_t oB = audio_chunk_get_channel(preallocated, ch);
    for (size_t i = 0; i < 1024; i++) {
      ASSERT_NEAR(oA[i], oB[i], 1e-12);
    }
  }
  audio_chunk_free(input);
  audio_chunk_free(out_alloc);
  audio_chunk_free(preallocated);
  audio_mixer_free(mixerA);
  audio_mixer_free(mixerB);
}

TEST(MixerInoutAPI_OverwritesPriorData) {
  mixer_source_t src0 = {.channel = 0, .gain = 0.0, .has_gain = true};
  mixer_mapping_t map = {.dest = 0, .sources_count = 1, .sources = &src0};
  mixer_config_t config = {
      .channels_in = 1, .channels_out = 1, .mapping_count = 1, .mapping = &map};
  audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);

  audio_chunk_t* output = make_constant_chunk(16, 1, 99.0);
  output->valid_frames = 0;

  audio_chunk_t* input = audio_chunk_create(4, 1);
  mutable_waveform_t b0 = audio_chunk_get_channel(input, 0);
  b0[0] = 1.0;
  b0[1] = 2.0;
  b0[2] = 3.0;
  b0[3] = 4.0;

  mixer_error_t err = audio_mixer_process(mixer, input, output);
  ASSERT_EQ(MIXER_OK, err);
  ASSERT_EQ(4, output->valid_frames);
  waveform_t o0 = audio_chunk_get_channel(output, 0);
  ASSERT_NEAR(1.0, o0[0], 1e-12);
  ASSERT_NEAR(2.0, o0[1], 1e-12);
  ASSERT_NEAR(3.0, o0[2], 1e-12);
  ASSERT_NEAR(4.0, o0[3], 1e-12);

  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_mixer_free(mixer);
}

TEST(MixerInoutAPI_RejectsTooSmallOutputBuffer) {
  mixer_source_t src0 = {.channel = 0, .gain = 0.0, .has_gain = true};
  mixer_mapping_t map = {.dest = 0, .sources_count = 1, .sources = &src0};
  mixer_config_t config = {
      .channels_in = 1, .channels_out = 1, .mapping_count = 1, .mapping = &map};
  audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);

  audio_chunk_t* input = make_constant_chunk(256, 1, 1.0);
  audio_chunk_t* output = make_constant_chunk(16, 1, 0.0);
  output->valid_frames = 0;

  mixer_error_t err = audio_mixer_process(mixer, input, output);
  ASSERT_EQ(MIXER_ERR_OUTPUT_BUFFER_TOO_SMALL, err);

  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_mixer_free(mixer);
}

TEST(MixerInoutAPI_RejectsChannelMismatch) {
  mixer_source_t src0 = {.channel = 0, .gain = 0.0, .has_gain = true};
  mixer_mapping_t map = {.dest = 0, .sources_count = 1, .sources = &src0};
  mixer_config_t config = {
      .channels_in = 1, .channels_out = 2, .mapping_count = 1, .mapping = &map};
  audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);

  audio_chunk_t* input = make_constant_chunk(3, 1, 1.0);
  audio_chunk_t* output = make_constant_chunk(8, 1, 0.0);

  mixer_error_t err = audio_mixer_process(mixer, input, output);
  ASSERT_EQ(MIXER_ERR_CHANNEL_COUNT_MISMATCH, err);

  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_mixer_free(mixer);
}

TEST_MAIN()
