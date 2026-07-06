#include "test_support.h"
#include "../../Sources/CDSP/Filters/lookahead_limiter.h"
#include "../../Sources/CDSP/Filters/limiter.h"
#include <math.h>

static bool is_close(double left, double right, double maxdiff) {
    return fabs(left - right) < maxdiff;
}

static bool compare_waveforms(const double* left, const double* right, size_t count, double maxdiff) {
    for (size_t i = 0; i < count; i++) {
        if (!is_close(left[i], right[i], maxdiff)) return false;
    }
    return true;
}

TEST(test_lookahead_limiter_basic) {
    lookahead_limiter_parameters_t params = {
        .limit = 0.0,
        .attack = 4.0,
        .release = 1.0 / log(2.0),
        .unit = DELAY_UNIT_SAMPLES
    };
    lookahead_limiter_filter_t* filter = lookahead_limiter_filter_create("lookahead_limiter", &params, 48000, 1024);
    ASSERT_TRUE(filter != NULL);

    double input[] = {
        1.0, 1.0, 1.0, 1.0, 1.0, 2.0, -2.0, 1.0, 1.0, 2.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0
    };
    double expected[] = {
        0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.875, 0.75, 0.625, 1.0, -1.0,
        pow(0.5, 1.0 / 2.0), 0.625, 1.0, pow(0.5, 1.0 / 2.0), pow(0.5, 1.0 / 4.0),
        pow(0.5, 1.0 / 8.0), pow(0.5, 1.0 / 16.0), pow(0.5, 1.0 / 32.0)
    };

    lookahead_limiter_filter_process(filter, input, 19);
    ASSERT_TRUE(compare_waveforms(input, expected, 19, 1e-6));
    lookahead_limiter_filter_free(filter);
}

TEST(test_lookahead_limiter_same_as_limiter) {
    lookahead_limiter_parameters_t params_lookahead = {
        .limit = 0.0, .attack = 0.0, .release = 0.0, .unit = DELAY_UNIT_SAMPLES
    };
    lookahead_limiter_filter_t* filter_lookahead = lookahead_limiter_filter_create("lookahead", &params_lookahead, 48000, 1024);
    ASSERT_TRUE(filter_lookahead != NULL);

    limiter_parameters_t params_limiter = { .clip_limit = 0.0, .soft_clip = false };
    limiter_filter_t* filter_limiter = limiter_filter_create("limiter", &params_limiter);
    ASSERT_TRUE(filter_limiter != NULL);

    double lookahead_input[] = {0.5, 1.0, 2.0, -2.0, -1.0, -0.5, 1.5, -1.5, 0.0};
    double limiter_input[] = {0.5, 1.0, 2.0, -2.0, -1.0, -0.5, 1.5, -1.5, 0.0};

    lookahead_limiter_filter_process(filter_lookahead, lookahead_input, 9);
    limiter_filter_process(filter_limiter, limiter_input, 9);

    for (size_t i = 0; i < 9; i++) {
        ASSERT_DOUBLE_EQ(limiter_input[i], lookahead_input[i]);
    }

    lookahead_limiter_filter_free(filter_lookahead);
    limiter_filter_free(filter_limiter);
}

TEST(test_lookahead_limiter_zero_release) {
    lookahead_limiter_parameters_t params = {
        .limit = 0.0, .attack = 2.0, .release = 0.0, .unit = DELAY_UNIT_SAMPLES
    };
    lookahead_limiter_filter_t* filter = lookahead_limiter_filter_create("lookahead", &params, 48000, 1024);
    ASSERT_TRUE(filter != NULL);
    double input[] = {1.0, 1.0, 1.0, 2.0, 2.0, 2.0, 2.0, 2.0, 1.0, 1.0, 1.0};
    lookahead_limiter_filter_process(filter, input, 11);
    for (size_t i = 0; i < 11; i++) {
        ASSERT_TRUE(fabs(input[i]) <= 1.0);
    }
    lookahead_limiter_filter_free(filter);
}

TEST(test_lookahead_limiter_state_persistence) {
    lookahead_limiter_parameters_t params = {
        .limit = 0.0, .attack = 5.0, .release = 1.0 / log(2.0), .unit = DELAY_UNIT_SAMPLES
    };
    lookahead_limiter_filter_t* filter = lookahead_limiter_filter_create("lookahead", &params, 48000, 1024);
    ASSERT_TRUE(filter != NULL);

    double buf1[] = {1.0, 1.0, 1.0, 1.0, 1.0, 2.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    double expected1[] = {0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.9, 0.8, 0.7, 0.6, 1.0};
    lookahead_limiter_filter_process(filter, buf1, 11);
    ASSERT_TRUE(compare_waveforms(buf1, expected1, 11, 1e-6));

    double buf2[] = {1.0, 1.0, 1.0, 1.0};
    double expected2[] = {
        pow(0.5, 1.0 / 2.0), pow(0.5, 1.0 / 4.0), pow(0.5, 1.0 / 8.0), pow(0.5, 1.0 / 16.0)
    };
    lookahead_limiter_filter_process(filter, buf2, 4);
    ASSERT_TRUE(compare_waveforms(buf2, expected2, 4, 1e-6));

    lookahead_limiter_filter_free(filter);
}

TEST(test_lookahead_limiter_attack_over_one_second_rejected) {
    lookahead_limiter_parameters_t params = {
        .limit = 0.0, .attack = 48001.0, .release = 4.0, .unit = DELAY_UNIT_SAMPLES
    };
    ASSERT_NE(0, lookahead_limiter_parameters_validate(&params, 48000, NULL));
}

TEST(test_lookahead_limiter_chunksize_larger_than_samplerate) {
    lookahead_limiter_parameters_t params = {
        .limit = 0.0, .attack = 4.0, .release = 1.0, .unit = DELAY_UNIT_SAMPLES
    };
    lookahead_limiter_filter_t* filter = lookahead_limiter_filter_create("lookahead", &params, 4, 8);
    ASSERT_TRUE(filter != NULL);
    double input[] = {1.0, 1.0, 2.0, 1.0, 1.0, -2.0, 1.0, 1.0};
    lookahead_limiter_filter_process(filter, input, 8);
    lookahead_limiter_filter_free(filter);
}

TEST_MAIN()
