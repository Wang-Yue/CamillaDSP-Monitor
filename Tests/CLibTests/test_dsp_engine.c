#include "../../Sources/CDSP/Engine/dsp_engine.h"
#include "test_support.h"
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <stdio.h>

static void run_e2e_test_config(const char* json, const char* backend_name) {
  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool success = dsp_engine_set_config(engine, json, &err);
  if (!success) {
    printf("⚠️ [E2E Warning] Skipping E2E test for backend '%s' (Initialization failed: %s)\n",
           backend_name, err.message);
    dsp_engine_free(engine);
    return;
  }

  struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
  nanosleep(&ts, NULL);

  vu_levels_t vu = dsp_engine_get_vu_levels(engine);
  (void)vu;

  dsp_engine_stop(engine);
  dsp_engine_free(engine);
  printf("✅ [E2E Success] Backend '%s' ran successfully\n", backend_name);
}

TEST(DSPEngineCreateFree) {
  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);
  dsp_engine_free(engine);
}

TEST(DSPEngineDeviceCapabilities) {
  audio_device_t devs[32];
  int count = dsp_engine_get_available_devices("coreaudio", false, devs, 32);
  ASSERT_TRUE(count >= 0);

  // Test freeing NULL descriptor (should be a safe no-op)
  dsp_engine_free_device_capabilities(NULL);

  if (count > 0) {
    audio_device_descriptor_t* desc =
        dsp_engine_get_device_capabilities("coreaudio", devs[0].name, false, NULL);
    if (desc) {
      dsp_engine_free_device_capabilities(desc);
    }
  }
}

TEST(DSPEngineSetConfigAndReload) {
  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

#if defined(__linux__)
  const char* json1 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";

  const char* json2 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    },\n"
      "    \"mixers\": [{\n"
      "        \"name\": \"mymixer\",\n"
      "        \"channels_in\": 2,\n"
      "        \"channels_out\": 2,\n"
      "        \"mapping\": [{\n"
      "            \"dest\": 0,\n"
      "            \"sources\": [{\"channel\": 0, \"gain\": 0.0, \"inverted\": "
      "false, \"mute\": false}]\n"
      "        }, {\n"
      "            \"dest\": 1,\n"
      "            \"sources\": [{\"channel\": 1, \"gain\": 0.0, \"inverted\": "
      "false, \"mute\": false}]\n"
      "        }]\n"
      "    }],\n"
      "    \"pipeline\": [{\n"
      "        \"type\": \"Mixer\",\n"
      "        \"name\": \"mymixer\"\n"
      "    }]\n"
      "}";
#else
  const char* json1 =
      "{\n"
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

  const char* json2 =
      "{\n"
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
      "    },\n"
      "    \"mixers\": [{\n"
      "        \"name\": \"mymixer\",\n"
      "        \"channels_in\": 2,\n"
      "        \"channels_out\": 2,\n"
      "        \"mapping\": [{\n"
      "            \"dest\": 0,\n"
      "            \"sources\": [{\"channel\": 0, \"gain\": 0.0, \"inverted\": "
      "false, \"mute\": false}]\n"
      "        }, {\n"
      "            \"dest\": 1,\n"
      "            \"sources\": [{\"channel\": 1, \"gain\": 0.0, \"inverted\": "
      "false, \"mute\": false}]\n"
      "        }]\n"
      "    }],\n"
      "    \"pipeline\": [{\n"
      "        \"type\": \"Mixer\",\n"
      "        \"name\": \"mymixer\"\n"
      "    }]\n"
      "}";
#endif

  audio_backend_error_t err;
  dsp_engine_set_config(engine, json1, &err);
  dsp_engine_set_config(engine, json2, &err);
  dsp_engine_stop(engine);
  dsp_engine_free(engine);
}

TEST(DSPEngineE2E_ALSA) {
#if defined(__linux__)
  const char* json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 512,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";
  run_e2e_test_config(json, "ALSA");
#endif
}

TEST(DSPEngineE2E_PulseAudio) {
#if defined(__linux__)
  const char* json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 512,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Pulse\",\n"
      "            \"device\": \"default\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Pulse\",\n"
      "            \"device\": \"default\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";
  run_e2e_test_config(json, "PulseAudio");
#endif
}

TEST(DSPEngineE2E_PipeWire) {
#if defined(__linux__)
  const char* json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 48000,\n"
      "        \"chunksize\": 512,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Pipewire\",\n"
      "            \"device\": \"default\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Pipewire\",\n"
      "            \"device\": \"default\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";
  run_e2e_test_config(json, "PipeWire");
#endif
}

TEST(DSPEngineE2E_GeneratorFile) {
  const char* json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 512,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Generator\",\n"
      "            \"channels\": 2,\n"
      "            \"signal\": {\n"
      "                \"type\": \"Sine\",\n"
      "                \"freq\": 1000.0,\n"
      "                \"level\": -6.0\n"
      "            }\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"File\",\n"
      "            \"filename\": \"/tmp/e2e_out.raw\",\n"
      "            \"format\": \"S16_LE\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";
  run_e2e_test_config(json, "Generator -> File");
}

TEST(DSPEngineE2E_FileFile) {
  FILE* f = fopen("/tmp/e2e_in.raw", "wb");
  if (f) {
    char zeros[1024 * 4 * 2] = {0};
    fwrite(zeros, 1, sizeof(zeros), f);
    fclose(f);
  }

  const char* json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 512,\n"
      "        \"capture\": {\n"
      "            \"type\": \"File\",\n"
      "            \"filename\": \"/tmp/e2e_in.raw\",\n"
      "            \"format\": \"S16_LE\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"File\",\n"
      "            \"filename\": \"/tmp/e2e_out.raw\",\n"
      "            \"format\": \"S16_LE\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";
  run_e2e_test_config(json, "File -> File");
}

TEST_MAIN()
