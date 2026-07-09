
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../../Sources/CDSP/Pipeline/config_loader.h"
#include "../../Sources/CDSP/Pipeline/pipeline.h"
#include "test_support.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void init_default_config(dsp_config_t* config) {
  memset(config, 0, sizeof(dsp_config_t));
  config->devices.samplerate = 44100;
  config->devices.chunksize = 1024;
#if defined(ENABLE_COREAUDIO)
  config->devices.capture.type = AUDIO_BACKEND_TYPE_CORE_AUDIO;
  config->devices.capture.cfg.coreaudio.channels = 2;
  config->devices.playback.type = AUDIO_BACKEND_TYPE_CORE_AUDIO;
  config->devices.playback.cfg.coreaudio.channels = 2;
#elif defined(ENABLE_ALSA)
  config->devices.capture.type = AUDIO_BACKEND_TYPE_ALSA;
  config->devices.capture.cfg.alsa.channels = 2;
  config->devices.playback.type = AUDIO_BACKEND_TYPE_ALSA;
  config->devices.playback.cfg.alsa.channels = 2;
#elif defined(ENABLE_WASAPI)
  config->devices.capture.type = AUDIO_BACKEND_TYPE_WASAPI;
  config->devices.capture.cfg.wasapi.channels = 2;
  config->devices.playback.type = AUDIO_BACKEND_TYPE_WASAPI;
  config->devices.playback.cfg.wasapi.channels = 2;
#else
  config->devices.capture.type = AUDIO_BACKEND_TYPE_FILE;
  config->devices.capture.cfg.raw_file.channels = 2;
  config->devices.playback.type = AUDIO_BACKEND_TYPE_FILE;
  config->devices.playback.cfg.raw_file.channels = 2;
#endif
}

TEST(PipelineInitEmpty) {
  dsp_config_t config;
  init_default_config(&config);
  processing_parameters_t* params = processing_parameters_create(2, 2);
  config_error_t err;
  config_error_init(&err);
  pipeline_t* pipeline = pipeline_create(&config, params, 0, &err);
  ASSERT_TRUE(pipeline != NULL);
  ASSERT_EQ(CONFIG_ERR_NONE, err.type);

  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineProcessPassthrough) {
  dsp_config_t config;
  init_default_config(&config);
  processing_parameters_t* params = processing_parameters_create(2, 2);
  pipeline_t* pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < 1024; t++) {
      buf[t] = sin(2.0 * M_PI * 1000.0 * (double)t / 44100.0);
    }
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_chunk_t* output = audio_chunk_create(1024, 2);
  pipeline_error_t err = pipeline_process(pipeline, chunk, output);
  ASSERT_EQ(PIPELINE_OK, err);
  ASSERT_EQ(1024, audio_chunk_get_valid_frames(output));
  ASSERT_EQ(2, audio_chunk_get_channels(output));

  for (size_t ch = 0; ch < 2; ch++) {
    waveform_t in_buf = audio_chunk_get_channel(chunk, ch);
    waveform_t out_buf = audio_chunk_get_channel(output, ch);
    for (size_t t = 0; t < 1024; t++) {
      ASSERT_NEAR(in_buf[t], out_buf[t], 1e-9);
    }
  }

  audio_chunk_free(chunk);
  audio_chunk_free(output);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineWithFilter) {
  dsp_config_t config;
  init_default_config(&config);

  named_filter_config_t filter_cfg;
  memset(&filter_cfg, 0, sizeof(filter_cfg));
  strcpy(filter_cfg.name, "mygain");
  filter_cfg.filter.type = FILTER_TYPE_GAIN;
  filter_cfg.filter.parameters.gain.gain = -6.0;
  filter_cfg.filter.parameters.gain.has_gain = true;
  filter_cfg.filter.parameters.gain.scale = GAIN_SCALE_DB;
  config.filters = &filter_cfg;
  config.filters_count = 1;

  char* filter_name = strdup("mygain");
  pipeline_step_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channel = 0;
  step.has_channel = true;
  step.names = &filter_name;
  step.names_count = 1;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t* params = processing_parameters_create(2, 2);
  pipeline_t* pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < 1024; t++) {
      buf[t] = 1.0;
    }
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_chunk_t* output = audio_chunk_create(1024, 2);
  pipeline_error_t err = pipeline_process(pipeline, chunk, output);
  ASSERT_EQ(PIPELINE_OK, err);

  waveform_t out0 = audio_chunk_get_channel(output, 0);
  waveform_t out1 = audio_chunk_get_channel(output, 1);
  ASSERT_NEAR(double_from_db(-6.0), out0[0], 1e-5);
  ASSERT_NEAR(1.0, out1[0], 1e-5);

  free(filter_name);
  audio_chunk_free(chunk);
  audio_chunk_free(output);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineWithMixer) {
  dsp_config_t config;
  init_default_config(&config);

  mixer_source_t src0 = {.channel = 1,
                         .gain = 0.0,
                         .has_gain = true,
                         .scale = GAIN_SCALE_DB,
                         .inverted = false,
                         .mute = false};
  mixer_source_t src1 = {.channel = 0,
                         .gain = 0.0,
                         .has_gain = true,
                         .scale = GAIN_SCALE_DB,
                         .inverted = false,
                         .mute = false};
  mixer_mapping_t maps[2] = {
      {.dest = 0, .sources_count = 1, .sources = &src0, .mute = false},
      {.dest = 1, .sources_count = 1, .sources = &src1, .mute = false}};
  named_mixer_config_t mixer_cfg;
  memset(&mixer_cfg, 0, sizeof(mixer_cfg));
  strcpy(mixer_cfg.name, "swap");
  mixer_cfg.mixer.channels_in = 2;
  mixer_cfg.mixer.channels_out = 2;
  mixer_cfg.mixer.mapping_count = 2;
  mixer_cfg.mixer.mapping = maps;
  config.mixers = &mixer_cfg;
  config.mixers_count = 1;

  pipeline_step_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_MIXER;
  strcpy(step.name, "swap");
  step.has_name = true;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t* params = processing_parameters_create(2, 2);
  pipeline_t* pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  mutable_waveform_t ch0 = audio_chunk_get_channel(chunk, 0);
  mutable_waveform_t ch1 = audio_chunk_get_channel(chunk, 1);
  for (size_t t = 0; t < 1024; t++) {
    ch0[t] = 1.0;
    ch1[t] = 2.0;
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_chunk_t* output = audio_chunk_create(1024, 2);
  pipeline_error_t err = pipeline_process(pipeline, chunk, output);
  ASSERT_EQ(PIPELINE_OK, err);

  waveform_t out0 = audio_chunk_get_channel(output, 0);
  waveform_t out1 = audio_chunk_get_channel(output, 1);
  ASSERT_NEAR(2.0, out0[0], 1e-5);
  ASSERT_NEAR(1.0, out1[0], 1e-5);

  audio_chunk_free(chunk);
  audio_chunk_free(output);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineBypassedFilter) {
  dsp_config_t config;
  init_default_config(&config);

  named_filter_config_t filter_cfg;
  memset(&filter_cfg, 0, sizeof(filter_cfg));
  strcpy(filter_cfg.name, "mygain");
  filter_cfg.filter.type = FILTER_TYPE_GAIN;
  filter_cfg.filter.parameters.gain.gain = -6.0;
  filter_cfg.filter.parameters.gain.has_gain = true;
  filter_cfg.filter.parameters.gain.scale = GAIN_SCALE_DB;
  config.filters = &filter_cfg;
  config.filters_count = 1;

  char* filter_name = strdup("mygain");
  pipeline_step_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channel = 0;
  step.has_channel = true;
  step.names = &filter_name;
  step.names_count = 1;
  step.bypassed = true;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t* params = processing_parameters_create(2, 2);
  pipeline_t* pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < 1024; t++) {
      buf[t] = 1.0;
    }
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_chunk_t* output = audio_chunk_create(1024, 2);
  pipeline_error_t err = pipeline_process(pipeline, chunk, output);
  ASSERT_EQ(PIPELINE_OK, err);

  waveform_t out0 = audio_chunk_get_channel(output, 0);
  ASSERT_NEAR(1.0, out0[0], 1e-5);

  free(filter_name);
  audio_chunk_free(chunk);
  audio_chunk_free(output);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineFilterChannelOutOfBounds) {
  dsp_config_t config;
  init_default_config(&config);

  named_filter_config_t filter_cfg;
  memset(&filter_cfg, 0, sizeof(filter_cfg));
  strcpy(filter_cfg.name, "mygain");
  filter_cfg.filter.type = FILTER_TYPE_GAIN;
  filter_cfg.filter.parameters.gain.gain = -6.0;
  filter_cfg.filter.parameters.gain.has_gain = true;
  filter_cfg.filter.parameters.gain.scale = GAIN_SCALE_DB;
  config.filters = &filter_cfg;
  config.filters_count = 1;

  char* filter_name = strdup("mygain");
  pipeline_step_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channel = 2;
  step.has_channel = true;
  step.names = &filter_name;
  step.names_count = 1;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t* params = processing_parameters_create(2, 2);
  pipeline_t* pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < 1024; t++) {
      buf[t] = 1.0;
    }
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_chunk_t* output = audio_chunk_create(1024, 2);
  pipeline_error_t err = pipeline_process(pipeline, chunk, output);
  ASSERT_EQ(PIPELINE_OK, err);

  waveform_t out0 = audio_chunk_get_channel(output, 0);
  waveform_t out1 = audio_chunk_get_channel(output, 1);
  ASSERT_NEAR(1.0, out0[0], 1e-5);
  ASSERT_NEAR(1.0, out1[0], 1e-5);

  free(filter_name);
  audio_chunk_free(chunk);
  audio_chunk_free(output);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineVolumeChange) {
  dsp_config_t config;
  init_default_config(&config);
  config.devices.volume_ramp_time = 0.0;
  config.devices.has_volume_ramp_time = true;

  processing_parameters_t* params = processing_parameters_create(2, 2);
  pipeline_t* pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  processing_parameters_set_target_volume(params, -10.0);

  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < 1024; t++) {
      buf[t] = 1.0;
    }
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_chunk_t* output = audio_chunk_create(1024, 2);
  pipeline_error_t err = pipeline_process(pipeline, chunk, output);
  ASSERT_EQ(PIPELINE_OK, err);

  waveform_t out0 = audio_chunk_get_channel(output, 0);
  ASSERT_NEAR(double_from_db(-10.0), out0[0], 1e-5);
  ASSERT_NEAR(double_from_db(-10.0), out0[1023], 1e-5);

  audio_chunk_free(chunk);
  audio_chunk_free(output);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineMute) {
  dsp_config_t config;
  init_default_config(&config);
  config.devices.volume_ramp_time = 0.0;
  config.devices.has_volume_ramp_time = true;

  processing_parameters_t* params = processing_parameters_create(2, 2);
  pipeline_t* pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  processing_parameters_set_muted(params, true);

  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < 1024; t++) {
      buf[t] = 1.0;
    }
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_chunk_t* output = audio_chunk_create(1024, 2);
  pipeline_error_t err = pipeline_process(pipeline, chunk, output);
  ASSERT_EQ(PIPELINE_OK, err);

  waveform_t out0 = audio_chunk_get_channel(output, 0);
  ASSERT_NEAR(0.0, out0[0], 1e-5);
  ASSERT_NEAR(0.0, out0[1023], 1e-5);

  audio_chunk_free(chunk);
  audio_chunk_free(output);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineVolumePresetBeforeBuild) {
  dsp_config_t config;
  init_default_config(&config);

  processing_parameters_t* params = processing_parameters_create(2, 2);
  processing_parameters_set_target_volume(params, -100.0);

  pipeline_t* pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < 1024; t++) {
      buf[t] = 1.0;
    }
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_chunk_t* output = audio_chunk_create(1024, 2);
  pipeline_error_t err = pipeline_process(pipeline, chunk, output);
  ASSERT_EQ(PIPELINE_OK, err);

  for (size_t ch = 0; ch < 2; ch++) {
    waveform_t out_buf = audio_chunk_get_channel(output, ch);
    for (size_t t = 0; t < 1024; t++) {
      ASSERT_TRUE(out_buf[t] < 1e-4);
    }
  }

  audio_chunk_free(chunk);
  audio_chunk_free(output);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineInitFilterMissingNames) {
  dsp_config_t config;
  init_default_config(&config);

  pipeline_step_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channel = 0;
  step.has_channel = true;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t* params = processing_parameters_create(2, 2);
  config_error_t err;
  config_error_init(&err);
  pipeline_t* pipeline = pipeline_create(&config, params, 0, &err);
  ASSERT_TRUE(pipeline == NULL);
  ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
  ASSERT_TRUE(strstr(err.message, "Filter step missing names") != NULL);

  processing_parameters_free(params);
}

TEST(PipelineInitFilterChannels) {
  dsp_config_t config;
  init_default_config(&config);

  named_filter_config_t filter_cfg;
  memset(&filter_cfg, 0, sizeof(filter_cfg));
  strcpy(filter_cfg.name, "mygain");
  filter_cfg.filter.type = FILTER_TYPE_GAIN;
  filter_cfg.filter.parameters.gain.gain = -6.0;
  filter_cfg.filter.parameters.gain.has_gain = true;
  config.filters = &filter_cfg;
  config.filters_count = 1;

  int chs[2] = {0, 1};
  char* filter_name = strdup("mygain");
  pipeline_step_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channels = chs;
  step.channels_count = 2;
  step.names = &filter_name;
  step.names_count = 1;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t* params = processing_parameters_create(2, 2);
  pipeline_t* pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  free(filter_name);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineInitFilterAllChannels) {
  dsp_config_t config;
  init_default_config(&config);

  named_filter_config_t filter_cfg;
  memset(&filter_cfg, 0, sizeof(filter_cfg));
  strcpy(filter_cfg.name, "mygain");
  filter_cfg.filter.type = FILTER_TYPE_GAIN;
  filter_cfg.filter.parameters.gain.gain = -6.0;
  filter_cfg.filter.parameters.gain.has_gain = true;
  config.filters = &filter_cfg;
  config.filters_count = 1;

  char* filter_name = strdup("mygain");
  pipeline_step_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.names = &filter_name;
  step.names_count = 1;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t* params = processing_parameters_create(2, 2);
  pipeline_t* pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  free(filter_name);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineInitFilterUndefined) {
  dsp_config_t config;
  init_default_config(&config);

  char* filter_name = strdup("undefined_filter");
  pipeline_step_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channel = 0;
  step.has_channel = true;
  step.names = &filter_name;
  step.names_count = 1;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t* params = processing_parameters_create(2, 2);
  config_error_t err;
  config_error_init(&err);
  pipeline_t* pipeline = pipeline_create(&config, params, 0, &err);
  ASSERT_TRUE(pipeline == NULL);
  ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
  ASSERT_TRUE(strstr(err.message, "not defined") != NULL);

  free(filter_name);
  processing_parameters_free(params);
}

TEST(PipelineInitMixerMissingName) {
  dsp_config_t config;
  init_default_config(&config);

  pipeline_step_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_MIXER;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t* params = processing_parameters_create(2, 2);
  config_error_t err;
  config_error_init(&err);
  pipeline_t* pipeline = pipeline_create(&config, params, 0, &err);
  ASSERT_TRUE(pipeline == NULL);
  ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
  ASSERT_TRUE(strstr(err.message, "Mixer step missing name or config") != NULL);

  processing_parameters_free(params);
}

TEST(PipelineWithLoudnessFilters) {
  dsp_config_t config;
  init_default_config(&config);

  named_filter_config_t filter_cfg;
  memset(&filter_cfg, 0, sizeof(filter_cfg));
  strcpy(filter_cfg.name, "myloud");
  filter_cfg.filter.type = FILTER_TYPE_LOUDNESS;
  filter_cfg.filter.parameters.loudness.reference_level = -20.0;
  filter_cfg.filter.parameters.loudness.has_reference_level = true;
  filter_cfg.filter.parameters.loudness.fader = FADER_MAIN;
  config.filters = &filter_cfg;
  config.filters_count = 1;

  char* filter_name = strdup("myloud");
  pipeline_step_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channel = 0;
  step.has_channel = true;
  step.names = &filter_name;
  step.names_count = 1;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t* params = processing_parameters_create(2, 2);
  pipeline_t* pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  free(filter_name);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineSequentialMixersZeroAllocationRecovery) {
  dsp_config_t config;
  init_default_config(&config);

  mixer_source_t src_2to4_0 = {
      .channel = 0, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB};
  mixer_source_t src_2to4_1 = {
      .channel = 1, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB};
  mixer_mapping_t map_2to4[4] = {
      {.dest = 0, .sources_count = 1, .sources = &src_2to4_0},
      {.dest = 1, .sources_count = 1, .sources = &src_2to4_0},
      {.dest = 2, .sources_count = 1, .sources = &src_2to4_1},
      {.dest = 3, .sources_count = 1, .sources = &src_2to4_1}};
  named_mixer_config_t mixer_cfgs[2];
  memset(mixer_cfgs, 0, sizeof(mixer_cfgs));
  strcpy(mixer_cfgs[0].name, "2to4");
  mixer_cfgs[0].mixer.channels_in = 2;
  mixer_cfgs[0].mixer.channels_out = 4;
  mixer_cfgs[0].mixer.mapping_count = 4;
  mixer_cfgs[0].mixer.mapping = map_2to4;

  mixer_source_t src_4to2_0[2] = {
      {.channel = 0, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB},
      {.channel = 2, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB}};
  mixer_source_t src_4to2_1[2] = {
      {.channel = 1, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB},
      {.channel = 3, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB}};
  mixer_mapping_t map_4to2[2] = {
      {.dest = 0, .sources_count = 2, .sources = src_4to2_0},
      {.dest = 1, .sources_count = 2, .sources = src_4to2_1}};
  strcpy(mixer_cfgs[1].name, "4to2");
  mixer_cfgs[1].mixer.channels_in = 4;
  mixer_cfgs[1].mixer.channels_out = 2;
  mixer_cfgs[1].mixer.mapping_count = 2;
  mixer_cfgs[1].mixer.mapping = map_4to2;

  config.mixers = mixer_cfgs;
  config.mixers_count = 2;

  pipeline_step_t steps[2];
  memset(steps, 0, sizeof(steps));
  steps[0].type = PIPELINE_STEP_TYPE_MIXER;
  strcpy(steps[0].name, "2to4");
  steps[0].has_name = true;
  steps[1].type = PIPELINE_STEP_TYPE_MIXER;
  strcpy(steps[1].name, "4to2");
  steps[1].has_name = true;
  config.pipeline = steps;
  config.pipeline_count = 2;

  processing_parameters_t* params = processing_parameters_create(2, 2);
  pipeline_t* pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  mutable_waveform_t ch0 = audio_chunk_get_channel(chunk, 0);
  mutable_waveform_t ch1 = audio_chunk_get_channel(chunk, 1);
  for (size_t t = 0; t < 1024; t++) {
    ch0[t] = 1.0;
    ch1[t] = 2.0;
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_chunk_t* output1 = audio_chunk_create(1024, 2);
  pipeline_error_t err = pipeline_process(pipeline, chunk, output1);
  ASSERT_EQ(PIPELINE_OK, err);
  ASSERT_EQ(2, audio_chunk_get_channels(output1));
  waveform_t out1_0 = audio_chunk_get_channel(output1, 0);
  waveform_t out1_1 = audio_chunk_get_channel(output1, 1);
  ASSERT_NEAR(3.0, out1_0[0], 1e-5);
  ASSERT_NEAR(3.0, out1_1[0], 1e-5);

  audio_chunk_t* chunk2 = audio_chunk_create(1024, 2);
  mutable_waveform_t ch2_0 = audio_chunk_get_channel(chunk2, 0);
  mutable_waveform_t ch2_1 = audio_chunk_get_channel(chunk2, 1);
  for (size_t t = 0; t < 1024; t++) {
    ch2_0[t] = 3.0;
    ch2_1[t] = 4.0;
  }
  audio_chunk_set_valid_frames(chunk2, 1024);

  audio_chunk_t* output2 = audio_chunk_create(1024, 2);
  err = pipeline_process(pipeline, chunk2, output2);
  ASSERT_EQ(PIPELINE_OK, err);
  ASSERT_EQ(2, audio_chunk_get_channels(output2));
  waveform_t out2_0 = audio_chunk_get_channel(output2, 0);
  ASSERT_NEAR(7.0, out2_0[0], 1e-5);

  audio_chunk_free(chunk);
  audio_chunk_free(output1);
  audio_chunk_free(chunk2);
  audio_chunk_free(output2);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineProcessValidationThrows) {
  dsp_config_t config;
  init_default_config(&config);
  processing_parameters_t* params = processing_parameters_create(2, 2);
  pipeline_t* pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  audio_chunk_t* input = audio_chunk_create(1024, 2);
  audio_chunk_set_valid_frames(input, 1024);
  audio_chunk_t* output = audio_chunk_create(1024, 2);

  // 1. inputSizeMismatch
  audio_chunk_t* tooLargeInput = audio_chunk_create(2048, 2);
  audio_chunk_set_valid_frames(tooLargeInput, 2048);
  pipeline_error_t err = pipeline_process(pipeline, tooLargeInput, output);
  ASSERT_EQ(PIPELINE_ERR_INPUT_SIZE_MISMATCH, err);
  ASSERT_EQ(1024, pipeline_get_last_error_needed(pipeline));
  ASSERT_EQ(2048, pipeline_get_last_error_got(pipeline));

  // 2. input channel Count mismatch
  audio_chunk_t* wrongInputChannels = audio_chunk_create(1024, 1);
  audio_chunk_set_valid_frames(wrongInputChannels, 1024);
  err = pipeline_process(pipeline, wrongInputChannels, output);
  ASSERT_EQ(PIPELINE_ERR_CHANNEL_COUNT_MISMATCH, err);
  ASSERT_EQ(2, pipeline_get_last_error_needed(pipeline));
  ASSERT_EQ(1, pipeline_get_last_error_got(pipeline));

  // 3. output channel Count mismatch
  audio_chunk_t* wrongOutputChannels = audio_chunk_create(1024, 3);
  err = pipeline_process(pipeline, input, wrongOutputChannels);
  ASSERT_EQ(PIPELINE_ERR_CHANNEL_COUNT_MISMATCH, err);
  ASSERT_EQ(2, pipeline_get_last_error_needed(pipeline));
  ASSERT_EQ(3, pipeline_get_last_error_got(pipeline));

  // 4. output capacity too small
  audio_chunk_t* tooSmallOutput = audio_chunk_create(512, 2);
  err = pipeline_process(pipeline, input, tooSmallOutput);
  ASSERT_EQ(PIPELINE_ERR_OUTPUT_BUFFER_TOO_SMALL, err);
  ASSERT_EQ(1024, pipeline_get_last_error_needed(pipeline));
  ASSERT_EQ(512, pipeline_get_last_error_got(pipeline));

  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_chunk_free(tooLargeInput);
  audio_chunk_free(wrongInputChannels);
  audio_chunk_free(wrongOutputChannels);
  audio_chunk_free(tooSmallOutput);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(ConfigLoaderParseAndValidate) {
  const char* json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"File\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"File\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";
  dsp_config_t* config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = config_loader_parse(json, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);

  res = dsp_config_validate(config, &err);
  ASSERT_EQ(0, res);
  ASSERT_EQ(CONFIG_ERR_NONE, err.type);

  dsp_config_free(config);

  dsp_config_t invalid_config;
  init_default_config(&invalid_config);
  invalid_config.devices.samplerate = 0;
  res = dsp_config_validate(&invalid_config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_VALIDATION, err.type);
}

TEST(PipelineReload_StatePreserved) {
  dsp_config_t config;
  init_default_config(&config);

  // 1. Setup Peak EQ Biquad Filter config
  named_filter_config_t filter_cfg;
  memset(&filter_cfg, 0, sizeof(filter_cfg));
  strcpy(filter_cfg.name, "mybiquad");
  filter_cfg.filter.type = FILTER_TYPE_BIQUAD;
  filter_cfg.filter.parameters.biquad.type = BIQUAD_TYPE_PEAKING;
  filter_cfg.filter.parameters.biquad.freq = 1000.0;
  filter_cfg.filter.parameters.biquad.gain = 12.0;
  filter_cfg.filter.parameters.biquad.q = 1.0;
  filter_cfg.filter.parameters.biquad.steepness_type = STEEPNESS_TYPE_Q;
  config.filters = &filter_cfg;
  config.filters_count = 1;

  char* filter_name = strdup("mybiquad");
  pipeline_step_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channel = 0;
  step.has_channel = true;
  step.names = &filter_name;
  step.names_count = 1;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t* params = processing_parameters_create(2, 2);
  
  // 2. Create pipeline 1
  pipeline_t* pipeline1 = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline1 != NULL);

  // 3. Prepare an impulse chunk (1.0 at index 0, then zeros)
  audio_chunk_t* impulse_chunk = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(impulse_chunk, ch);
    buf[0] = 1.0;
    for (size_t t = 1; t < 1024; t++) {
      buf[t] = 0.0;
    }
  }
  audio_chunk_set_valid_frames(impulse_chunk, 1024);

  // 4. Prepare a zero chunk (for decay)
  audio_chunk_t* zero_chunk = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(zero_chunk, ch);
    for (size_t t = 0; t < 1024; t++) {
      buf[t] = 0.0;
    }
  }
  audio_chunk_set_valid_frames(zero_chunk, 1024);

  // 5. Process impulse through pipeline 1 (this updates internal z1, z2 state)
  audio_chunk_t* output_chunk1_1 = audio_chunk_create(1024, 2);
  pipeline_error_t perr = pipeline_process(pipeline1, impulse_chunk, output_chunk1_1);
  ASSERT_EQ(PIPELINE_OK, perr);

  // 6. Process zero chunk through pipeline 1 (natural decay output)
  audio_chunk_t* output_chunk1_2 = audio_chunk_create(1024, 2);
  perr = pipeline_process(pipeline1, zero_chunk, output_chunk1_2);
  ASSERT_EQ(PIPELINE_OK, perr);

  // Read first decay sample
  waveform_t dec_out1_2 = audio_chunk_get_channel(output_chunk1_2, 0);
  double expected_first_decay_sample = dec_out1_2[0];
  // Ensure it's not zero (otherwise the test is trivial/faulty)
  ASSERT_NE(0.0, expected_first_decay_sample);

  // 7. Create pipeline 2 (a rebuilt pipeline with same config)
  pipeline_t* pipeline2 = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline2 != NULL);

  // 8. Process zero chunk through pipeline 2 (without state transfer)
  audio_chunk_t* output_chunk2 = audio_chunk_create(1024, 2);
  perr = pipeline_process(pipeline2, zero_chunk, output_chunk2);
  ASSERT_EQ(PIPELINE_OK, perr);

  // Without state transfer, the output should be exactly 0.0
  waveform_t out_chunk2 = audio_chunk_get_channel(output_chunk2, 0);
  ASSERT_EQ(0.0, out_chunk2[0]);

  // 9. Create pipeline 3 (another rebuilt pipeline)
  pipeline_t* pipeline3 = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline3 != NULL);

  // 10. Transfer state from pipeline1 (after it processed chunk 1) to pipeline3
  pipeline_transfer_state(pipeline3, pipeline1);

  // 11. Process zero chunk through pipeline 3 (should seamlessly continue decay)
  audio_chunk_t* output_chunk3 = audio_chunk_create(1024, 2);
  perr = pipeline_process(pipeline3, zero_chunk, output_chunk3);
  ASSERT_EQ(PIPELINE_OK, perr);

  waveform_t out_chunk3 = audio_chunk_get_channel(output_chunk3, 0);
  // Output should match the expected decay value!
  ASSERT_NEAR(expected_first_decay_sample, out_chunk3[0], 1e-12);

  // Cleanup
  free(filter_name);
  pipeline_free(pipeline1);
  pipeline_free(pipeline2);
  pipeline_free(pipeline3);
  audio_chunk_free(impulse_chunk);
  audio_chunk_free(zero_chunk);
  audio_chunk_free(output_chunk1_1);
  audio_chunk_free(output_chunk1_2);
  audio_chunk_free(output_chunk2);
  audio_chunk_free(output_chunk3);
  processing_parameters_free(params);
}

TEST_MAIN()
