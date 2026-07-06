#include "test_support.h"
#include "../../Sources/CDSP/Engine/dsp_engine.h"

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
        audio_device_descriptor_t* desc = dsp_engine_get_device_capabilities("coreaudio", devs[0].name, false);
        if (desc) {
            dsp_engine_free_device_capabilities(desc);
        }
    }
}

TEST(DSPEngineSetConfigAndReload) {
    dsp_engine_t* engine = dsp_engine_create();
    ASSERT_TRUE(engine != NULL);

#if defined(__linux__)
    const char* json1 = "{\n"
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

    const char* json2 = "{\n"
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
        "            \"sources\": [{\"channel\": 0, \"gain\": 0.0, \"inverted\": false, \"mute\": false}]\n"
        "        }, {\n"
        "            \"dest\": 1,\n"
        "            \"sources\": [{\"channel\": 1, \"gain\": 0.0, \"inverted\": false, \"mute\": false}]\n"
        "        }]\n"
        "    }],\n"
        "    \"pipeline\": [{\n"
        "        \"type\": \"Mixer\",\n"
        "        \"name\": \"mymixer\"\n"
        "    }]\n"
        "}";
#else
    const char* json1 = "{\n"
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

    const char* json2 = "{\n"
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
        "            \"sources\": [{\"channel\": 0, \"gain\": 0.0, \"inverted\": false, \"mute\": false}]\n"
        "        }, {\n"
        "            \"dest\": 1,\n"
        "            \"sources\": [{\"channel\": 1, \"gain\": 0.0, \"inverted\": false, \"mute\": false}]\n"
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

TEST_MAIN()
