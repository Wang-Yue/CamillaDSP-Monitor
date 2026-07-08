#include "../../Sources/CDSP/Audio/silence_counter.h"
#include "test_support.h"

TEST(DisabledWhenTimeoutZero) {
  silence_counter_t* counter = silence_counter_create(-40.0, 0.0, 48000, 1024);
  for (int i = 0; i < 10; i++) {
    ASSERT_EQ(PROCESSING_STATE_RUNNING,
              silence_counter_update(counter, -100.0));
  }
  silence_counter_free(counter);
}

TEST(StaysRunningUntilLimitReached) {
  silence_counter_t* counter = silence_counter_create(-40.0, 1.0, 48000, 1024);
  size_t limit = 47;
  ASSERT_EQ(limit, silence_counter_get_limit_chunks(counter));
  for (size_t i = 0; i < limit - 1; i++) {
    ASSERT_EQ(PROCESSING_STATE_RUNNING,
              silence_counter_update(counter, -100.0));
  }
  ASSERT_EQ(PROCESSING_STATE_PAUSED, silence_counter_update(counter, -100.0));
  silence_counter_free(counter);
}

TEST(RecoversWhenSignalReturns) {
  silence_counter_t* counter = silence_counter_create(-40.0, 0.5, 48000, 1024);
  for (int i = 0; i < 60; i++) {
    silence_counter_update(counter, -100.0);
  }
  ASSERT_EQ(PROCESSING_STATE_PAUSED, silence_counter_update(counter, -100.0));
  ASSERT_EQ(PROCESSING_STATE_RUNNING, silence_counter_update(counter, -10.0));
  ASSERT_EQ(PROCESSING_STATE_RUNNING, silence_counter_update(counter, -5.0));
  silence_counter_free(counter);
}

TEST(ThresholdIsExclusive) {
  silence_counter_t* counter = silence_counter_create(-40.0, 1.0, 48000, 1024);
  for (int i = 0; i < 10; i++) {
    silence_counter_update(counter, -40.0);
  }
  ASSERT_EQ(10, silence_counter_get_silent_chunks(counter));
  silence_counter_update(counter, -39.99);
  ASSERT_EQ(0, silence_counter_get_silent_chunks(counter));
  silence_counter_free(counter);
}

TEST_MAIN()
