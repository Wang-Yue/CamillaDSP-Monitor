#if defined(__linux__)
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <string.h>

#include "../../Sources/CDSP/Config/config_diff.h"
#include "../../Sources/CDSP/Config/configuration.h"
#include "test_support.h"

static const char* base_json =
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
    "    },\n"
    "    \"filters\": {\n"
    "        \"my_gain\": {\n"
    "            \"type\": \"Gain\",\n"
    "            \"parameters\": {\n"
    "                \"gain\": -6.0\n"
    "            }\n"
    "        }\n"
    "    },\n"
    "    \"pipeline\": [\n"
    "        {\n"
    "            \"type\": \"Filter\",\n"
    "            \"channel\": 0,\n"
    "            \"names\": [\"my_gain\"]\n"
    "        }\n"
    "    ]\n"
    "}";

TEST(ConfigDiffEqual) {
  dsp_config_t* c1 = NULL;
  dsp_config_t* c2 = NULL;
  config_error_t err;
  config_error_init(&err);

  int r1 = dsp_config_parse_json(base_json, &c1, &err);
  int r2 = dsp_config_parse_json(base_json, &c2, &err);
  ASSERT_EQ(0, r1);
  ASSERT_EQ(0, r2);

  config_change_t* change = config_change_create();
  ASSERT_TRUE(change != NULL);
  config_change_type_t res = config_diff(c1, c2, change);
  ASSERT_EQ(CONFIG_CHANGE_NONE, res);

  size_t filters_count = 0;
  char** filters = config_change_take_filters(change, &filters_count);
  ASSERT_EQ(0, filters_count);
  ASSERT_TRUE(filters == NULL);

  config_change_free(change);
  dsp_config_free(c1);
  dsp_config_free(c2);
}

TEST(ConfigDiffDevices) {
  const char* json_diff =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 48000,\n"
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

  dsp_config_t* c1 = NULL;
  dsp_config_t* c2 = NULL;
  config_error_t err;
  config_error_init(&err);

  int r1 = dsp_config_parse_json(base_json, &c1, &err);
  int r2 = dsp_config_parse_json(json_diff, &c2, &err);
  ASSERT_EQ(0, r1);
  ASSERT_EQ(0, r2);

  config_change_t* change = config_change_create();
  ASSERT_TRUE(change != NULL);
  config_change_type_t res = config_diff(c1, c2, change);
  ASSERT_EQ(CONFIG_CHANGE_DEVICES, res);

  config_change_free(change);
  dsp_config_free(c1);
  dsp_config_free(c2);
}

TEST(ConfigDiffFilterParams) {
  const char* json_diff =
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
      "    },\n"
      "    \"filters\": {\n"
      "        \"my_gain\": {\n"
      "            \"type\": \"Gain\",\n"
      "            \"parameters\": {\n"
      "                \"gain\": -3.0\n"
      "            }\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [\n"
      "        {\n"
      "            \"type\": \"Filter\",\n"
      "            \"channel\": 0,\n"
      "            \"names\": [\"my_gain\"]\n"
      "        }\n"
      "    ]\n"
      "}";

  dsp_config_t* c1 = NULL;
  dsp_config_t* c2 = NULL;
  config_error_t err;
  config_error_init(&err);

  int r1 = dsp_config_parse_json(base_json, &c1, &err);
  int r2 = dsp_config_parse_json(json_diff, &c2, &err);
  ASSERT_EQ(0, r1);
  ASSERT_EQ(0, r2);

  config_change_t* change = config_change_create();
  ASSERT_TRUE(change != NULL);
  config_change_type_t res = config_diff(c1, c2, change);
  ASSERT_EQ(CONFIG_CHANGE_FILTER_PARAMETERS, res);

  size_t filters_count = 0;
  char** filters = config_change_take_filters(change, &filters_count);
  ASSERT_EQ(1, filters_count);
  ASSERT_STR_EQ("my_gain", filters[0]);

  // Clean up returned name list since ownership was transferred to us
  for (size_t i = 0; i < filters_count; i++) {
    free(filters[i]);
  }
  free(filters);

  config_change_free(change);
  dsp_config_free(c1);
  dsp_config_free(c2);
}

TEST_MAIN()
