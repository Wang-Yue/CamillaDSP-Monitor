#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../Sources/CDSP/Pipeline/state_file.h"
#include "test_support.h"

TEST(test_state_file_round_trip) {
  const char* test_file = "test_state.yaml";

  dsp_state_t* original = dsp_state_create();
  ASSERT_TRUE(original != NULL);
  dsp_state_set_config_path(original, "/var/tmp/config.json");
  dsp_state_set_mute(original, 0, true);
  dsp_state_set_mute(original, 1, false);
  dsp_state_set_mute(original, 2, true);
  dsp_state_set_mute(original, 3, false);
  dsp_state_set_mute(original, 4, true);
  dsp_state_set_volume(original, 0, 0.0);
  dsp_state_set_volume(original, 1, -6.02);
  dsp_state_set_volume(original, 2, -12.0);
  dsp_state_set_volume(original, 3, -20.5);
  dsp_state_set_volume(original, 4, 3.14159);

  // Save state
  ASSERT_TRUE(dsp_state_save(test_file, original));

  // Load state
  dsp_state_t* loaded = dsp_state_create();
  ASSERT_TRUE(loaded != NULL);
  ASSERT_TRUE(dsp_state_load(test_file, loaded));

  // Check config path
  ASSERT_TRUE(dsp_state_has_config_path(loaded));
  ASSERT_STR_EQ(dsp_state_get_config_path(original),
                dsp_state_get_config_path(loaded));

  // Check mutes and volumes
  for (int i = 0; i < 5; i++) {
    ASSERT_TRUE(dsp_state_get_mute(original, i) ==
                dsp_state_get_mute(loaded, i));
    ASSERT_NEAR(dsp_state_get_volume(original, i),
                dsp_state_get_volume(loaded, i), 1e-6);
  }

  dsp_state_free(original);
  dsp_state_free(loaded);

  // Clean up
  unlink(test_file);
}

TEST(test_state_file_no_config_path) {
  const char* test_file = "test_state_no_path.yaml";

  dsp_state_t* original = dsp_state_create();
  ASSERT_TRUE(original != NULL);
  dsp_state_set_has_config_path(original, false);
  dsp_state_set_mute(original, 0, false);
  dsp_state_set_mute(original, 1, true);
  dsp_state_set_mute(original, 2, false);
  dsp_state_set_mute(original, 3, true);
  dsp_state_set_mute(original, 4, false);
  dsp_state_set_volume(original, 0, -1.0);
  dsp_state_set_volume(original, 1, -2.0);
  dsp_state_set_volume(original, 2, -3.0);
  dsp_state_set_volume(original, 3, -4.0);
  dsp_state_set_volume(original, 4, -5.0);

  // Save state
  ASSERT_TRUE(dsp_state_save(test_file, original));

  // Load state
  dsp_state_t* loaded = dsp_state_create();
  ASSERT_TRUE(loaded != NULL);
  ASSERT_TRUE(dsp_state_load(test_file, loaded));

  // Check config path
  ASSERT_FALSE(dsp_state_has_config_path(loaded));

  // Check mutes and volumes
  for (int i = 0; i < 5; i++) {
    ASSERT_TRUE(dsp_state_get_mute(original, i) ==
                dsp_state_get_mute(loaded, i));
    ASSERT_NEAR(dsp_state_get_volume(original, i),
                dsp_state_get_volume(loaded, i), 1e-6);
  }

  dsp_state_free(original);
  dsp_state_free(loaded);

  // Clean up
  unlink(test_file);
}

TEST_MAIN()
