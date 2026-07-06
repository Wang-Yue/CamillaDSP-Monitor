#include "../CTests/test_support.h"
#include "../CLib/Engine/rate_controller.h"
#include <unistd.h>
#include <math.h>

TEST(ReturnsUnitySpeedAtTarget) {
    pi_rate_controller_t* pi = pi_rate_controller_create_default(48000, 1.0, 1024);
    ASSERT_TRUE(pi != NULL);
    double speed = pi_rate_controller_next(pi, 1024.0);
    ASSERT_NEAR(1.0, speed, 1e-12);
    pi_rate_controller_free(pi);
}

TEST(HighBufferTriggersSlowdown) {
    pi_rate_controller_t* pi = pi_rate_controller_create_default(48000, 1.0, 1024);
    ASSERT_TRUE(pi != NULL);
    double last_speed = 1.0;
    for (int i = 0; i < 25; i++) {
        last_speed = pi_rate_controller_next(pi, 4096.0);
    }
    ASSERT_TRUE(last_speed < 1.0);
    ASSERT_TRUE(last_speed >= 1.0 - 0.005);
    pi_rate_controller_free(pi);
}

TEST(LowBufferTriggersSpeedup) {
    pi_rate_controller_t* pi = pi_rate_controller_create_default(48000, 1.0, 1024);
    ASSERT_TRUE(pi != NULL);
    double last_speed = 1.0;
    for (int i = 0; i < 25; i++) {
        last_speed = pi_rate_controller_next(pi, 256.0);
    }
    ASSERT_TRUE(last_speed > 1.0);
    ASSERT_TRUE(last_speed <= 1.0 + 0.005);
    pi_rate_controller_free(pi);
}

TEST(OutputAlwaysClampedWithin5PerMille) {
    pi_rate_controller_t* pi = pi_rate_controller_create_default(48000, 1.0, 1024);
    ASSERT_TRUE(pi != NULL);
    for (int i = 0; i < 200; i++) {
        pi_rate_controller_next(pi, 1000000.0);
    }
    double speed = pi_rate_controller_next(pi, 1000000.0);
    ASSERT_TRUE(speed >= 1.0 - 0.005);
    ASSERT_TRUE(speed <= 1.0 + 0.005);
    pi_rate_controller_free(pi);
}

TEST(AveragerReturnsNilWhenEmpty) {
    averager_t avg;
    averager_init(&avg);
    double val = 0.0;
    ASSERT_FALSE(averager_get_average(&avg, &val));
}

TEST(AveragerComputesMean) {
    averager_t avg;
    averager_init(&avg);
    averager_add(&avg, 10.0);
    averager_add(&avg, 20.0);
    averager_add(&avg, 30.0);
    double val = 0.0;
    ASSERT_TRUE(averager_get_average(&avg, &val));
    ASSERT_NEAR(20.0, val, 1e-12);
}

TEST(AveragerRestartClearsState) {
    averager_t avg;
    averager_init(&avg);
    averager_add(&avg, 100.0);
    averager_restart(&avg);
    double val = 0.0;
    ASSERT_FALSE(averager_get_average(&avg, &val));
    averager_add(&avg, 5.0);
    ASSERT_TRUE(averager_get_average(&avg, &val));
    ASSERT_DOUBLE_EQ(5.0, val);
}

TEST(StopwatchElapsesMonotonically) {
    stopwatch_t sw;
    stopwatch_init(&sw);
    double t0 = stopwatch_elapsed_seconds(&sw);
    usleep(10000); // 10 ms
    double t1 = stopwatch_elapsed_seconds(&sw);
    ASSERT_TRUE(t1 > t0);
    stopwatch_restart(&sw);
    double t2 = stopwatch_elapsed_seconds(&sw);
    ASSERT_TRUE(t2 < t1);
}

TEST_MAIN()
