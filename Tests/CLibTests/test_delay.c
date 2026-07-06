#include "test_support.h"
#include "../../Sources/CDSP/Filters/delay.h"
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

TEST(delay_small) {
    double waveform[] = {0.0, -0.5, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    double waveform_delayed[] = {0.0, 0.0, 0.0, 0.0, -0.5, 1.0, 0.0, 0.0};
    delay_parameters_t params = { .delay = 3.0, .unit = DELAY_UNIT_SAMPLES, .subsample = false };
    delay_filter_t* filter = delay_filter_create("delay", &params, 44100);
    ASSERT_TRUE(filter != NULL);
    delay_filter_process(filter, waveform, 8);
    for (size_t i = 0; i < 8; i++) {
        ASSERT_DOUBLE_EQ(waveform_delayed[i], waveform[i]);
    }
    delay_filter_free(filter);
}

TEST(delay_supersmall) {
    double waveform[] = {0.0, -0.5, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    double waveform_delayed[] = {0.0, -0.5, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    delay_parameters_t params = { .delay = 0.1, .unit = DELAY_UNIT_SAMPLES, .subsample = false };
    delay_filter_t* filter = delay_filter_create("delay", &params, 44100);
    ASSERT_TRUE(filter != NULL);
    delay_filter_process(filter, waveform, 8);
    for (size_t i = 0; i < 8; i++) {
        ASSERT_DOUBLE_EQ(waveform_delayed[i], waveform[i]);
    }
    delay_filter_free(filter);
}

TEST(delay_large) {
    double waveform1[] = {0.0, -0.5, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    double waveform2[] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    double waveform_delayed[] = {0.0, 0.0, -0.5, 1.0, 0.0, 0.0, 0.0, 0.0};
    delay_parameters_t params = { .delay = 9.0, .unit = DELAY_UNIT_SAMPLES, .subsample = false };
    delay_filter_t* filter = delay_filter_create("delay", &params, 44100);
    ASSERT_TRUE(filter != NULL);
    delay_filter_process(filter, waveform1, 8);
    delay_filter_process(filter, waveform2, 8);
    for (size_t i = 0; i < 8; i++) {
        ASSERT_DOUBLE_EQ(0.0, waveform1[i]);
        ASSERT_DOUBLE_EQ(waveform_delayed[i], waveform2[i]);
    }
    delay_filter_free(filter);
}

TEST(delay_fraction) {
    double waveform[] = {0.0, -0.5, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    double expected_waveform[] = {
        0.0,
        0.01051051051051051,
        -0.13446780113446782,
        -0.2476751025299573,
        1.0522122611990257,
        -0.23903133046978262,
        0.07523664949897024,
        -0.021743938066703532,
        0.006413537427714274,
        -0.001882310318672015,
    };
    delay_parameters_t params = { .delay = 1.7, .unit = DELAY_UNIT_SAMPLES, .subsample = true };
    delay_filter_t* filter = delay_filter_create("delay", &params, 44100);
    ASSERT_TRUE(filter != NULL);
    delay_filter_process(filter, waveform, 10);
    ASSERT_TRUE(compare_waveforms(waveform, expected_waveform, 10, 1.0e-6));
    delay_filter_free(filter);
}

TEST_MAIN()
