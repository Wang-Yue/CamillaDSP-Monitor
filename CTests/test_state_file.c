#include "../CTests/test_support.h"
#include "../CLib/Pipeline/state_file.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

TEST(test_state_file_round_trip) {
    const char* test_file = "test_state.yaml";
    
    dsp_state_t original;
    memset(&original, 0, sizeof(original));
    strcpy(original.config_path, "/var/tmp/config.json");
    original.has_config_path = true;
    original.mute[0] = true;
    original.mute[1] = false;
    original.mute[2] = true;
    original.mute[3] = false;
    original.mute[4] = true;
    original.volume[0] = 0.0;
    original.volume[1] = -6.02;
    original.volume[2] = -12.0;
    original.volume[3] = -20.5;
    original.volume[4] = 3.14159;

    // Save state
    ASSERT_TRUE(dsp_state_save(test_file, &original));

    // Load state
    dsp_state_t loaded;
    ASSERT_TRUE(dsp_state_load(test_file, &loaded));

    // Check config path
    ASSERT_TRUE(loaded.has_config_path);
    ASSERT_STR_EQ(original.config_path, loaded.config_path);

    // Check mutes and volumes
    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(original.mute[i] == loaded.mute[i]);
        ASSERT_NEAR(original.volume[i], loaded.volume[i], 1e-6);
    }

    // Clean up
    unlink(test_file);
}

TEST(test_state_file_no_config_path) {
    const char* test_file = "test_state_no_path.yaml";
    
    dsp_state_t original;
    memset(&original, 0, sizeof(original));
    original.has_config_path = false;
    original.mute[0] = false;
    original.mute[1] = true;
    original.mute[2] = false;
    original.mute[3] = true;
    original.mute[4] = false;
    original.volume[0] = -1.0;
    original.volume[1] = -2.0;
    original.volume[2] = -3.0;
    original.volume[3] = -4.0;
    original.volume[4] = -5.0;

    // Save state
    ASSERT_TRUE(dsp_state_save(test_file, &original));

    // Load state
    dsp_state_t loaded;
    ASSERT_TRUE(dsp_state_load(test_file, &loaded));

    // Check config path
    ASSERT_FALSE(loaded.has_config_path);

    // Check mutes and volumes
    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(original.mute[i] == loaded.mute[i]);
        ASSERT_NEAR(original.volume[i], loaded.volume[i], 1e-6);
    }

    // Clean up
    unlink(test_file);
}

TEST_MAIN()
