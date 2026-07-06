#include "../CTests/test_support.h"
#include "../CLib/Config/configuration.h"
#include <string.h>
#include <stdlib.h>

TEST(ParseValidConfig) {
    const char* json = "{\n"
        "    \"devices\": {\n"
        "        \"samplerate\": 44100,\n"
        "        \"chunksize\": 1024,\n"
        "        \"capture\": {\n"
        "            \"type\": \"CoreAudio\",\n"
        "            \"channels\": 2\n"
        "        },\n"
        "        \"playback\": {\n"
        "            \"type\": \"CoreAudio\",\n"
        "            \"channels\": 2\n"
        "        }\n"
        "    }\n"
        "}";
    dsp_config_t* config = NULL;
    config_error_t err;
    config_error_init(&err);
    int res = dsp_config_parse_json(json, &config, &err);
    ASSERT_EQ(0, res);
    ASSERT_TRUE(config != NULL);
    ASSERT_EQ(44100, config->devices.samplerate);
    ASSERT_EQ(1024, config->devices.chunksize);
    ASSERT_EQ(2, config->devices.capture.channels);
    ASSERT_EQ(2, config->devices.playback.channels);
    dsp_config_free(config);
}

TEST(ParseResamplerConfig) {
    const char* json = "{\n"
        "    \"devices\": {\n"
        "        \"samplerate\": 48000,\n"
        "        \"chunksize\": 1024,\n"
        "        \"capture_samplerate\": 44100,\n"
        "        \"resampler\": {\n"
        "            \"type\": \"AsyncSinc\",\n"
        "            \"profile\": \"Balanced\",\n"
        "            \"interpolation\": \"Cubic\",\n"
        "            \"sinc_len\": 256,\n"
        "            \"oversampling_factor\": 512,\n"
        "            \"window\": \"BlackmanHarris2\",\n"
        "            \"f_cutoff\": 0.95\n"
        "        },\n"
        "        \"capture\": {\n"
        "            \"type\": \"CoreAudio\",\n"
        "            \"channels\": 2\n"
        "        },\n"
        "        \"playback\": {\n"
        "            \"type\": \"CoreAudio\",\n"
        "            \"channels\": 2\n"
        "        }\n"
        "    }\n"
        "}";
    dsp_config_t* config = NULL;
    config_error_t err;
    config_error_init(&err);
    int res = dsp_config_parse_json(json, &config, &err);
    ASSERT_EQ(0, res);
    ASSERT_TRUE(config != NULL);
    ASSERT_EQ(48000, config->devices.samplerate);
    ASSERT_EQ(44100, config->devices.capture_samplerate);
    ASSERT_TRUE(config->devices.has_resampler);
    ASSERT_EQ(RESAMPLER_TYPE_ASYNC_SINC, config->devices.resampler.type);
    ASSERT_TRUE(config->devices.resampler.has_profile);
    ASSERT_STR_EQ("Balanced", config->devices.resampler.profile);
    ASSERT_TRUE(config->devices.resampler.has_interpolation);
    ASSERT_STR_EQ("Cubic", config->devices.resampler.interpolation);
    ASSERT_EQ(256, config->devices.resampler.sinc_len);
    ASSERT_EQ(512, config->devices.resampler.oversampling_factor);
    ASSERT_TRUE(config->devices.resampler.has_window);
    ASSERT_STR_EQ("BlackmanHarris2", config->devices.resampler.window);
    ASSERT_NEAR(0.95, config->devices.resampler.f_cutoff, 1e-6);
    dsp_config_free(config);
}

TEST(ParseInvalidJSON) {
    const char* json = "{\n"
        "    \"devices\": {\n"
        "        \"samplerate\": 44100,\n"
        "        \"chunksize\": 1024,\n"
        "        \"capture\": {\n"
        "            \"type\": \"CoreAudio\",\n"
        "            \"channels\": 2\n";
    dsp_config_t* config = NULL;
    config_error_t err;
    config_error_init(&err);
    int res = dsp_config_parse_json(json, &config, &err);
    ASSERT_NE(0, res);
    ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
    if (config) dsp_config_free(config);
}

TEST(ValidateSampleRate) {
    dsp_config_t config;
    memset(&config, 0, sizeof(config));
    config.devices.samplerate = 0;
    config.devices.chunksize = 1024;
    config.devices.capture.channels = 2;
    config.devices.playback.channels = 2;
    config_error_t err;
    config_error_init(&err);
    int res = dsp_config_validate(&config, &err);
    ASSERT_NE(0, res);
    ASSERT_EQ(CONFIG_ERR_VALIDATION, err.type);
    ASSERT_TRUE(strstr(err.message, "Sample rate must be positive") != NULL);
}

TEST(ValidateChunkSize) {
    dsp_config_t config;
    memset(&config, 0, sizeof(config));
    config.devices.samplerate = 44100;
    config.devices.chunksize = 0;
    config.devices.capture.channels = 2;
    config.devices.playback.channels = 2;
    config_error_t err;
    config_error_init(&err);
    int res = dsp_config_validate(&config, &err);
    ASSERT_NE(0, res);
    ASSERT_EQ(CONFIG_ERR_VALIDATION, err.type);
    ASSERT_TRUE(strstr(err.message, "Chunk size must be positive") != NULL);
}

TEST(ValidateChannels) {
    dsp_config_t config;
    memset(&config, 0, sizeof(config));
    config.devices.samplerate = 44100;
    config.devices.chunksize = 1024;
    config.devices.capture.channels = 0;
    config.devices.playback.channels = 2;
    config_error_t err;
    config_error_init(&err);
    int res = dsp_config_validate(&config, &err);
    ASSERT_NE(0, res);
    ASSERT_EQ(CONFIG_ERR_VALIDATION, err.type);
    ASSERT_TRUE(strstr(err.message, "Capture channels must be positive") != NULL);

    config.devices.capture.channels = 2;
    config.devices.playback.channels = 0;
    res = dsp_config_validate(&config, &err);
    ASSERT_NE(0, res);
    ASSERT_EQ(CONFIG_ERR_VALIDATION, err.type);
    ASSERT_TRUE(strstr(err.message, "Playback channels must be positive") != NULL);
}

TEST(ValidatePipelineFilterMissingNames) {
    dsp_config_t config;
    memset(&config, 0, sizeof(config));
    config.devices.samplerate = 44100;
    config.devices.chunksize = 1024;
    config.devices.capture.channels = 2;
    config.devices.playback.channels = 2;
    
    pipeline_step_t step;
    memset(&step, 0, sizeof(step));
    step.type = PIPELINE_STEP_TYPE_FILTER;
    step.channel = 0;
    step.has_channel = true;
    
    config.pipeline = &step;
    config.pipeline_count = 1;
    
    config_error_t err;
    config_error_init(&err);
    int res = dsp_config_validate(&config, &err);
    ASSERT_NE(0, res);
    ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
    ASSERT_TRUE(strstr(err.message, "must have 'names'") != NULL);
}

TEST(ValidatePipelineFilterMissingChannels) {
    dsp_config_t config;
    memset(&config, 0, sizeof(config));
    config.devices.samplerate = 44100;
    config.devices.chunksize = 1024;
    config.devices.capture.channels = 2;
    config.devices.playback.channels = 2;
    
    char* name = strdup("myfilter");
    pipeline_step_t step;
    memset(&step, 0, sizeof(step));
    step.type = PIPELINE_STEP_TYPE_FILTER;
    step.names = &name;
    step.names_count = 1;
    
    config.pipeline = &step;
    config.pipeline_count = 1;
    
    config_error_t err;
    config_error_init(&err);
    int res = dsp_config_validate(&config, &err);
    free(name);
    ASSERT_NE(0, res);
    ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
    ASSERT_TRUE(strstr(err.message, "must have 'channel' or 'channels'") != NULL);
}

TEST(ValidatePipelineFilterUndefined) {
    dsp_config_t config;
    memset(&config, 0, sizeof(config));
    config.devices.samplerate = 44100;
    config.devices.chunksize = 1024;
    config.devices.capture.channels = 2;
    config.devices.playback.channels = 2;
    
    char* name = strdup("undefined_filter");
    pipeline_step_t step;
    memset(&step, 0, sizeof(step));
    step.type = PIPELINE_STEP_TYPE_FILTER;
    step.channel = 0;
    step.has_channel = true;
    step.names = &name;
    step.names_count = 1;
    
    config.pipeline = &step;
    config.pipeline_count = 1;
    
    config_error_t err;
    config_error_init(&err);
    int res = dsp_config_validate(&config, &err);
    free(name);
    ASSERT_NE(0, res);
    ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
    ASSERT_TRUE(strstr(err.message, "referenced in pipeline but not defined") != NULL);
}

TEST(ValidatePipelineFilterChannelOutOfRange) {
    dsp_config_t config;
    memset(&config, 0, sizeof(config));
    config.devices.samplerate = 44100;
    config.devices.chunksize = 1024;
    config.devices.capture.channels = 2;
    config.devices.playback.channels = 2;
    
    named_filter_config_t nf;
    memset(&nf, 0, sizeof(nf));
    strcpy(nf.name, "myfilter");
    nf.filter.type = FILTER_TYPE_GAIN;
    
    config.filters = &nf;
    config.filters_count = 1;
    
    char* name = strdup("myfilter");
    pipeline_step_t step;
    memset(&step, 0, sizeof(step));
    step.type = PIPELINE_STEP_TYPE_FILTER;
    step.channel = 2;
    step.has_channel = true;
    step.names = &name;
    step.names_count = 1;
    
    config.pipeline = &step;
    config.pipeline_count = 1;
    
    config_error_t err;
    config_error_init(&err);
    int res = dsp_config_validate(&config, &err);
    free(name);
    ASSERT_NE(0, res);
    ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
    ASSERT_TRUE(strstr(err.message, "references channel 2 but pipeline only has 2") != NULL);
}

TEST(ValidatePipelineMixerMissingName) {
    dsp_config_t config;
    memset(&config, 0, sizeof(config));
    config.devices.samplerate = 44100;
    config.devices.chunksize = 1024;
    config.devices.capture.channels = 2;
    config.devices.playback.channels = 2;
    
    pipeline_step_t step;
    memset(&step, 0, sizeof(step));
    step.type = PIPELINE_STEP_TYPE_MIXER;
    
    config.pipeline = &step;
    config.pipeline_count = 1;
    
    config_error_t err;
    config_error_init(&err);
    int res = dsp_config_validate(&config, &err);
    ASSERT_NE(0, res);
    ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
    ASSERT_TRUE(strstr(err.message, "must have 'name'") != NULL);
}

TEST(ValidatePipelineMixerUndefined) {
    dsp_config_t config;
    memset(&config, 0, sizeof(config));
    config.devices.samplerate = 44100;
    config.devices.chunksize = 1024;
    config.devices.capture.channels = 2;
    config.devices.playback.channels = 2;
    
    pipeline_step_t step;
    memset(&step, 0, sizeof(step));
    step.type = PIPELINE_STEP_TYPE_MIXER;
    strcpy(step.name, "undefined_mixer");
    step.has_name = true;
    
    config.pipeline = &step;
    config.pipeline_count = 1;
    
    config_error_t err;
    config_error_init(&err);
    int res = dsp_config_validate(&config, &err);
    ASSERT_NE(0, res);
    ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
    ASSERT_TRUE(strstr(err.message, "referenced in pipeline but not defined") != NULL);
}

TEST(ValidatePipelineMixerInputMismatch) {
    dsp_config_t config;
    memset(&config, 0, sizeof(config));
    config.devices.samplerate = 44100;
    config.devices.chunksize = 1024;
    config.devices.capture.channels = 2;
    config.devices.playback.channels = 2;
    
    named_mixer_config_t nm;
    memset(&nm, 0, sizeof(nm));
    strcpy(nm.name, "mymixer");
    nm.mixer.channels_in = 3;
    nm.mixer.channels_out = 2;
    
    config.mixers = &nm;
    config.mixers_count = 1;
    
    pipeline_step_t step;
    memset(&step, 0, sizeof(step));
    step.type = PIPELINE_STEP_TYPE_MIXER;
    strcpy(step.name, "mymixer");
    step.has_name = true;
    
    config.pipeline = &step;
    config.pipeline_count = 1;
    
    config_error_t err;
    config_error_init(&err);
    int res = dsp_config_validate(&config, &err);
    ASSERT_NE(0, res);
    ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
    ASSERT_TRUE(strstr(err.message, "expects 3 input channel(s) but pipeline has 2") != NULL);
}

TEST(ValidatePipelineOutputMismatch) {
    dsp_config_t config;
    memset(&config, 0, sizeof(config));
    config.devices.samplerate = 44100;
    config.devices.chunksize = 1024;
    config.devices.capture.channels = 2;
    config.devices.playback.channels = 2;
    
    named_mixer_config_t nm;
    memset(&nm, 0, sizeof(nm));
    strcpy(nm.name, "mymixer");
    nm.mixer.channels_in = 2;
    nm.mixer.channels_out = 3;
    
    config.mixers = &nm;
    config.mixers_count = 1;
    
    pipeline_step_t step;
    memset(&step, 0, sizeof(step));
    step.type = PIPELINE_STEP_TYPE_MIXER;
    strcpy(step.name, "mymixer");
    step.has_name = true;
    
    config.pipeline = &step;
    config.pipeline_count = 1;
    
    config_error_t err;
    config_error_init(&err);
    int res = dsp_config_validate(&config, &err);
    ASSERT_NE(0, res);
    ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
    ASSERT_TRUE(strstr(err.message, "outputs 3 channel(s) but playback device expects 2") != NULL);
}

TEST(ValidatePipelineBypassedStep) {
    dsp_config_t config;
    memset(&config, 0, sizeof(config));
    config.devices.samplerate = 44100;
    config.devices.chunksize = 1024;
    config.devices.capture.channels = 2;
    config.devices.playback.channels = 2;
    
    named_filter_config_t nf;
    memset(&nf, 0, sizeof(nf));
    strcpy(nf.name, "myfilter");
    nf.filter.type = FILTER_TYPE_GAIN;
    
    config.filters = &nf;
    config.filters_count = 1;
    
    char* name = strdup("myfilter");
    pipeline_step_t step;
    memset(&step, 0, sizeof(step));
    step.type = PIPELINE_STEP_TYPE_FILTER;
    step.channel = 2;
    step.has_channel = true;
    step.names = &name;
    step.names_count = 1;
    step.bypassed = true;
    
    config.pipeline = &step;
    config.pipeline_count = 1;
    
    config_error_t err;
    config_error_init(&err);
    int res = dsp_config_validate(&config, &err);
    free(name);
    ASSERT_EQ(0, res);
}

TEST(ConfigErrorDescription) {
    config_error_t err;
    char buf[256];
    
    config_error_set(&err, CONFIG_ERR_PARSE, "test");
    config_error_description(&err, buf, sizeof(buf));
    ASSERT_STR_EQ("Parse error: test", buf);
    
    config_error_set(&err, CONFIG_ERR_VALIDATION, "test");
    config_error_description(&err, buf, sizeof(buf));
    ASSERT_STR_EQ("Validation error: test", buf);
    
    config_error_set(&err, CONFIG_ERR_INVALID_FILTER, "test");
    config_error_description(&err, buf, sizeof(buf));
    ASSERT_STR_EQ("Invalid filter: test", buf);
    
    config_error_set(&err, CONFIG_ERR_INVALID_MIXER, "test");
    config_error_description(&err, buf, sizeof(buf));
    ASSERT_STR_EQ("Invalid mixer: test", buf);
    
    config_error_set(&err, CONFIG_ERR_INVALID_PIPELINE, "test");
    config_error_description(&err, buf, sizeof(buf));
    ASSERT_STR_EQ("Invalid pipeline: test", buf);
}

TEST(MixerValidatorDestOutOfRange) {
    mixer_mapping_t mapping;
    memset(&mapping, 0, sizeof(mapping));
    mapping.dest = 2;
    
    mixer_config_t mixer;
    memset(&mixer, 0, sizeof(mixer));
    mixer.channels_in = 2;
    mixer.channels_out = 2;
    mixer.mapping = &mapping;
    mixer.mapping_count = 1;
    
    config_error_t err;
    config_error_init(&err);
    int res = mixer_config_validate(&mixer, &err);
    ASSERT_NE(0, res);
    ASSERT_EQ(CONFIG_ERR_INVALID_MIXER, err.type);
    ASSERT_TRUE(strstr(err.message, "mixer dest 2 >= channels_out 2") != NULL);
}

TEST(MixerValidatorDuplicateDest) {
    mixer_mapping_t mappings[2];
    memset(mappings, 0, sizeof(mappings));
    mappings[0].dest = 0;
    mappings[1].dest = 0;
    
    mixer_config_t mixer;
    memset(&mixer, 0, sizeof(mixer));
    mixer.channels_in = 2;
    mixer.channels_out = 2;
    mixer.mapping = mappings;
    mixer.mapping_count = 2;
    
    config_error_t err;
    config_error_init(&err);
    int res = mixer_config_validate(&mixer, &err);
    ASSERT_NE(0, res);
    ASSERT_EQ(CONFIG_ERR_INVALID_MIXER, err.type);
    ASSERT_TRUE(strstr(err.message, "mixer dest 0 mapped more than once") != NULL);
}

TEST(MixerValidatorSourceOutOfRange) {
    mixer_source_t src;
    memset(&src, 0, sizeof(src));
    src.channel = 2;
    
    mixer_mapping_t mapping;
    memset(&mapping, 0, sizeof(mapping));
    mapping.dest = 0;
    mapping.sources = &src;
    mapping.sources_count = 1;
    
    mixer_config_t mixer;
    memset(&mixer, 0, sizeof(mixer));
    mixer.channels_in = 2;
    mixer.channels_out = 2;
    mixer.mapping = &mapping;
    mixer.mapping_count = 1;
    
    config_error_t err;
    config_error_init(&err);
    int res = mixer_config_validate(&mixer, &err);
    ASSERT_NE(0, res);
    ASSERT_EQ(CONFIG_ERR_INVALID_MIXER, err.type);
    ASSERT_TRUE(strstr(err.message, "mixer source channel 2 >= channels_in 2") != NULL);
}

TEST(MixerValidatorDuplicateSource) {
    mixer_source_t srcs[2];
    memset(srcs, 0, sizeof(srcs));
    srcs[0].channel = 0;
    srcs[1].channel = 0;
    
    mixer_mapping_t mapping;
    memset(&mapping, 0, sizeof(mapping));
    mapping.dest = 0;
    mapping.sources = srcs;
    mapping.sources_count = 2;
    
    mixer_config_t mixer;
    memset(&mixer, 0, sizeof(mixer));
    mixer.channels_in = 2;
    mixer.channels_out = 2;
    mixer.mapping = &mapping;
    mixer.mapping_count = 1;
    
    config_error_t err;
    config_error_init(&err);
    int res = mixer_config_validate(&mixer, &err);
    ASSERT_NE(0, res);
    ASSERT_EQ(CONFIG_ERR_INVALID_MIXER, err.type);
    ASSERT_TRUE(strstr(err.message, "mixer source channel 0 listed more than once for dest 0") != NULL);
}

TEST(ValidateInvalidFilterConfig) {
    named_filter_config_t nf;
    memset(&nf, 0, sizeof(nf));
    strcpy(nf.name, "mygain");
    nf.filter.type = FILTER_TYPE_GAIN;
    nf.filter.parameters.gain.gain = 200.0;
    nf.filter.parameters.gain.has_gain = true;
    
    dsp_config_t config;
    memset(&config, 0, sizeof(config));
    config.devices.samplerate = 44100;
    config.devices.chunksize = 1024;
    config.devices.capture.channels = 2;
    config.devices.playback.channels = 2;
    config.filters = &nf;
    config.filters_count = 1;
    
    config_error_t err;
    config_error_init(&err);
    int res = dsp_config_validate(&config, &err);
    ASSERT_NE(0, res);
    ASSERT_EQ(CONFIG_ERR_INVALID_FILTER, err.type);
    ASSERT_TRUE(strstr(err.message, "gain must be in (-150, 150)") != NULL);
}

TEST(ValidateInvalidMixerConfig) {
    mixer_mapping_t mapping;
    memset(&mapping, 0, sizeof(mapping));
    mapping.dest = 5;
    
    named_mixer_config_t nm;
    memset(&nm, 0, sizeof(nm));
    strcpy(nm.name, "mymixer");
    nm.mixer.channels_in = 2;
    nm.mixer.channels_out = 2;
    nm.mixer.mapping = &mapping;
    nm.mixer.mapping_count = 1;
    
    dsp_config_t config;
    memset(&config, 0, sizeof(config));
    config.devices.samplerate = 44100;
    config.devices.chunksize = 1024;
    config.devices.capture.channels = 2;
    config.devices.playback.channels = 2;
    config.mixers = &nm;
    config.mixers_count = 1;
    
    config_error_t err;
    config_error_init(&err);
    int res = dsp_config_validate(&config, &err);
    ASSERT_NE(0, res);
    ASSERT_EQ(CONFIG_ERR_INVALID_MIXER, err.type);
    ASSERT_TRUE(strstr(err.message, "mixer dest 5 >= channels_out 2") != NULL);
}

TEST_MAIN()
