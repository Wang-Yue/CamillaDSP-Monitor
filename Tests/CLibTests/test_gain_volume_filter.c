#include <math.h>

#include "../../Sources/CDSP/Filters/gain.h"
#include "../../Sources/CDSP/Filters/volume.h"
#include "test_support.h"

TEST(GainInvert) {
  gain_parameters_t params = {
      .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB, .inverted = true};
  gain_filter_t* filter = gain_filter_create("gain", &params);
  ASSERT_TRUE(filter != NULL);
  double waveform[] = {-0.5, 0.0, 0.5};
  gain_filter_process(filter, waveform, 3);
  ASSERT_NEAR(0.5, waveform[0], 1e-10);
  ASSERT_NEAR(0.0, waveform[1], 1e-10);
  ASSERT_NEAR(-0.5, waveform[2], 1e-10);
  gain_filter_free(filter);
}

TEST(GainAmplify) {
  gain_parameters_t params = {
      .gain = 20.0, .has_gain = true, .scale = GAIN_SCALE_DB};
  gain_filter_t* filter = gain_filter_create("gain", &params);
  ASSERT_TRUE(filter != NULL);
  double waveform[] = {-0.5, 0.0, 0.5};
  gain_filter_process(filter, waveform, 3);
  ASSERT_NEAR(-5.0, waveform[0], 1e-6);
  ASSERT_NEAR(0.0, waveform[1], 1e-10);
  ASSERT_NEAR(5.0, waveform[2], 1e-6);
  gain_filter_free(filter);
}

TEST(GainMute) {
  gain_parameters_t params = {.gain = 0.0, .has_gain = true, .mute = true};
  gain_filter_t* filter = gain_filter_create("gain", &params);
  ASSERT_TRUE(filter != NULL);
  double waveform[] = {-0.5, 0.0, 0.5, 1.0, -1.0};
  gain_filter_process(filter, waveform, 5);
  for (size_t i = 0; i < 5; i++) {
    ASSERT_DOUBLE_EQ(0.0, waveform[i]);
  }
  gain_filter_free(filter);
}

TEST(GainLinearScale) {
  gain_parameters_t params = {
      .gain = 0.5, .has_gain = true, .scale = GAIN_SCALE_LINEAR};
  gain_filter_t* filter = gain_filter_create("gain", &params);
  ASSERT_TRUE(filter != NULL);
  double waveform[] = {1.0};
  gain_filter_process(filter, waveform, 1);
  ASSERT_NEAR(0.5, waveform[0], 1e-10);
  gain_filter_free(filter);
}

static void process_vol(volume_filter_t* filter, double* waveform,
                        size_t count) {
  volume_filter_prepare_chunk(filter);
  volume_filter_process(filter, waveform, count);
  volume_filter_advance_ramp(filter);
}

TEST(VolumeUnityGain) {
  processing_parameters_t* proc_params = processing_parameters_create(2, 2);
  processing_parameters_set_target_volume_for_fader(proc_params, 0.0,
                                                    FADER_MAIN);
  volume_parameters_t params = {.ramp_time = 0.0,
                                .has_ramp_time = true,
                                .limit = 50.0,
                                .has_limit = true,
                                .fader = FADER_MAIN};
  volume_filter_t* filter =
      volume_filter_create("volume", &params, 44100, 4, proc_params);
  ASSERT_TRUE(filter != NULL);

  double waveform[] = {1.0, -0.5, 0.25, 0.0};
  double original[] = {1.0, -0.5, 0.25, 0.0};
  process_vol(filter, waveform, 4);
  for (size_t i = 0; i < 4; i++) {
    ASSERT_NEAR(original[i], waveform[i], 1e-10);
  }
  volume_filter_free(filter);
  processing_parameters_free(proc_params);
}

TEST(VolumeAttenuation) {
  processing_parameters_t* proc_params = processing_parameters_create(2, 2);
  processing_parameters_set_target_volume_for_fader(proc_params, -20.0,
                                                    FADER_MAIN);
  volume_parameters_t params = {.ramp_time = 0.0,
                                .has_ramp_time = true,
                                .limit = 50.0,
                                .has_limit = true,
                                .fader = FADER_MAIN};
  volume_filter_t* filter =
      volume_filter_create("volume", &params, 44100, 4, proc_params);
  ASSERT_TRUE(filter != NULL);

  double waveform[] = {1.0, -1.0, 0.5, -0.5};
  process_vol(filter, waveform, 4);
  double gain = double_from_db(-20.0);
  ASSERT_NEAR(1.0 * gain, waveform[0], 1e-10);
  ASSERT_NEAR(-1.0 * gain, waveform[1], 1e-10);
  ASSERT_NEAR(0.5 * gain, waveform[2], 1e-10);
  ASSERT_NEAR(-0.5 * gain, waveform[3], 1e-10);
  volume_filter_free(filter);
  processing_parameters_free(proc_params);
}

TEST(VolumeMuteRampsToZero) {
  processing_parameters_t* proc_params = processing_parameters_create(2, 2);
  processing_parameters_set_target_volume_for_fader(proc_params, 0.0,
                                                    FADER_MAIN);
  processing_parameters_set_muted_for_fader(proc_params, true, FADER_MAIN);
  volume_parameters_t params = {.ramp_time = 0.0,
                                .has_ramp_time = true,
                                .limit = 50.0,
                                .has_limit = true,
                                .fader = FADER_MAIN};
  volume_filter_t* filter =
      volume_filter_create("volume", &params, 44100, 4, proc_params);
  ASSERT_TRUE(filter != NULL);

  double waveform[] = {1.0, 0.5, -0.5, -1.0};
  process_vol(filter, waveform, 4);
  for (size_t i = 0; i < 4; i++) {
    ASSERT_NEAR(0.0, waveform[i], 1e-10);
  }
  volume_filter_free(filter);
  processing_parameters_free(proc_params);
}

TEST(VolumeRamp) {
  size_t chunk_size = 4;
  int sample_rate = 44100;
  double ramp_time_ms = 1000.0 * (double)chunk_size / (double)sample_rate * 2.0;

  processing_parameters_t* proc_params = processing_parameters_create(2, 2);
  processing_parameters_set_target_volume_for_fader(proc_params, 0.0,
                                                    FADER_MAIN);
  volume_parameters_t params = {.ramp_time = ramp_time_ms,
                                .has_ramp_time = true,
                                .limit = 50.0,
                                .has_limit = true,
                                .fader = FADER_MAIN};
  volume_filter_t* filter = volume_filter_create("volume", &params, sample_rate,
                                                 chunk_size, proc_params);
  ASSERT_TRUE(filter != NULL);

  double chunk0[] = {1.0, 1.0, 1.0, 1.0};
  process_vol(filter, chunk0, 4);
  for (size_t i = 0; i < chunk_size; i++) {
    ASSERT_NEAR(1.0, chunk0[i], 1e-10);
  }

  processing_parameters_set_target_volume_for_fader(proc_params, -20.0,
                                                    FADER_MAIN);

  double chunk1[] = {1.0, 1.0, 1.0, 1.0};
  process_vol(filter, chunk1, 4);

  double gain0db = double_from_db(0.0);
  double gain_m20db = double_from_db(-20.0);
  for (size_t i = 0; i < chunk_size; i++) {
    ASSERT_TRUE(chunk1[i] <= gain0db + 1e-6);
    ASSERT_TRUE(chunk1[i] >= gain_m20db - 1e-6);
  }
  ASSERT_TRUE(chunk1[0] > chunk1[chunk_size - 1]);

  double chunk2[] = {1.0, 1.0, 1.0, 1.0};
  process_vol(filter, chunk2, 4);
  ASSERT_TRUE(chunk2[chunk_size - 1] < chunk1[chunk_size - 1]);
  ASSERT_TRUE(chunk2[chunk_size - 1] >= gain_m20db - 1e-6);

  double chunk3[] = {1.0, 1.0, 1.0, 1.0};
  process_vol(filter, chunk3, 4);
  for (size_t i = 0; i < chunk_size; i++) {
    ASSERT_NEAR(gain_m20db, chunk3[i], 1e-6);
  }
  volume_filter_free(filter);
  processing_parameters_free(proc_params);
}

TEST(VolumeChangeThreshold) {
  processing_parameters_t* proc_params = processing_parameters_create(2, 2);
  processing_parameters_set_target_volume_for_fader(proc_params, 0.0,
                                                    FADER_MAIN);
  volume_parameters_t params = {.ramp_time = 0.0,
                                .has_ramp_time = true,
                                .limit = 50.0,
                                .has_limit = true,
                                .fader = FADER_MAIN};
  volume_filter_t* filter =
      volume_filter_create("volume", &params, 44100, 4, proc_params);
  ASSERT_TRUE(filter != NULL);

  double wave1[] = {1.0, 1.0, 1.0, 1.0};
  process_vol(filter, wave1, 4);

  processing_parameters_set_target_volume_for_fader(proc_params, 0.005,
                                                    FADER_MAIN);
  double wave2[] = {1.0, 1.0, 1.0, 1.0};
  process_vol(filter, wave2, 4);
  for (size_t i = 0; i < 4; i++) {
    ASSERT_NEAR(1.0, wave2[i], 1e-10);
  }

  processing_parameters_set_target_volume_for_fader(proc_params, 0.02,
                                                    FADER_MAIN);
  double wave3[] = {1.0, 1.0, 1.0, 1.0};
  process_vol(filter, wave3, 4);
  double expected_gain = double_from_db(0.02);
  for (size_t i = 0; i < 4; i++) {
    ASSERT_NEAR(expected_gain, wave3[i], 1e-6);
  }
  volume_filter_free(filter);
  processing_parameters_free(proc_params);
}

TEST(VolumeLimit) {
  processing_parameters_t* proc_params = processing_parameters_create(2, 2);
  processing_parameters_set_target_volume_for_fader(proc_params, 0.0,
                                                    FADER_MAIN);
  volume_parameters_t params = {.ramp_time = 0.0,
                                .has_ramp_time = true,
                                .limit = 10.0,
                                .has_limit = true,
                                .fader = FADER_MAIN};
  volume_filter_t* filter =
      volume_filter_create("volume", &params, 44100, 4, proc_params);
  ASSERT_TRUE(filter != NULL);

  processing_parameters_set_target_volume_for_fader(proc_params, 20.0,
                                                    FADER_MAIN);
  double waveform[] = {1.0, 1.0, 1.0, 1.0};
  process_vol(filter, waveform, 4);

  double expected_gain = double_from_db(10.0);
  for (size_t i = 0; i < 4; i++) {
    ASSERT_NEAR(expected_gain, waveform[i], 1e-6);
  }
  volume_filter_free(filter);
  processing_parameters_free(proc_params);
}

TEST(VolumeUpdateParametersClampsToLimit) {
  processing_parameters_t* proc_params = processing_parameters_create(2, 2);
  processing_parameters_set_target_volume_for_fader(proc_params, 20.0,
                                                    FADER_MAIN);
  volume_parameters_t params = {.ramp_time = 0.0,
                                .has_ramp_time = true,
                                .limit = 50.0,
                                .has_limit = true,
                                .fader = FADER_MAIN};
  volume_filter_t* filter =
      volume_filter_create("volume", &params, 44100, 4, proc_params);
  ASSERT_TRUE(filter != NULL);

  double wave1[] = {1.0, 1.0, 1.0, 1.0};
  process_vol(filter, wave1, 4);

  filter_config_t cfg;
  cfg.type = FILTER_TYPE_VOLUME;
  cfg.parameters.volume.ramp_time = 0.0;
  cfg.parameters.volume.has_ramp_time = true;
  cfg.parameters.volume.limit = 10.0;
  cfg.parameters.volume.has_limit = true;
  cfg.parameters.volume.fader = FADER_MAIN;
  volume_filter_update_parameters(filter, &cfg, 44100);

  processing_parameters_set_target_volume_for_fader(proc_params, 10.0,
                                                    FADER_MAIN);
  double wave2[] = {1.0, 1.0, 1.0, 1.0};
  process_vol(filter, wave2, 4);

  double expected_gain = double_from_db(10.0);
  for (size_t i = 0; i < 4; i++) {
    ASSERT_NEAR(expected_gain, wave2[i], 1e-6);
  }
  volume_filter_free(filter);
  processing_parameters_free(proc_params);
}

TEST_MAIN()
