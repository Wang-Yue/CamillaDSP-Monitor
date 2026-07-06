#include "test_support.h"
#include "../../Sources/CDSP/Filters/dither.h"
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

TEST(test_quantize) {
    double waveform[] = {-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0};
    double waveform2[] = {-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0};
    dither_parameters_t params = { .type = DITHER_TYPE_NONE, .bits = 8 };
    dither_filter_t* filter = dither_filter_create("dither", &params);
    ASSERT_TRUE(filter != NULL);
    dither_filter_process(filter, waveform, 7);

    ASSERT_TRUE(compare_waveforms(waveform, waveform2, 7, 1.0 / 128.0));
    ASSERT_TRUE(is_close(round(128.0 * waveform[2]), 128.0 * waveform[2], 1e-9));
    dither_filter_free(filter);
}

TEST(test_flat) {
    double waveform[] = {-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0};
    double waveform2[] = {-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0};
    dither_parameters_t params = { .type = DITHER_TYPE_FLAT, .bits = 8, .amplitude = 2.0, .has_amplitude = true };
    dither_filter_t* filter = dither_filter_create("dither", &params);
    ASSERT_TRUE(filter != NULL);
    dither_filter_process(filter, waveform, 7);

    ASSERT_TRUE(compare_waveforms(waveform, waveform2, 7, 1.0 / 64.0));
    ASSERT_TRUE(is_close(round(128.0 * waveform[2]), 128.0 * waveform[2], 1e-9));
    dither_filter_free(filter);
}

TEST(test_high_pass) {
    double waveform[] = {-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0};
    double waveform2[] = {-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0};
    dither_parameters_t params = { .type = DITHER_TYPE_HIGHPASS, .bits = 8 };
    dither_filter_t* filter = dither_filter_create("dither", &params);
    ASSERT_TRUE(filter != NULL);
    dither_filter_process(filter, waveform, 7);

    ASSERT_TRUE(compare_waveforms(waveform, waveform2, 7, 1.0 / 32.0));
    ASSERT_TRUE(is_close(round(128.0 * waveform[2]), 128.0 * waveform[2], 1e-9));
    dither_filter_free(filter);
}

TEST(test_lip) {
    double waveform[] = {-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0};
    double waveform2[] = {-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0};
    dither_parameters_t params = { .type = DITHER_TYPE_LIPSHITZ_441, .bits = 8 };
    dither_filter_t* filter = dither_filter_create("dither", &params);
    ASSERT_TRUE(filter != NULL);
    dither_filter_process(filter, waveform, 7);

    ASSERT_TRUE(compare_waveforms(waveform, waveform2, 7, 1.0 / 16.0));
    ASSERT_TRUE(is_close(round(128.0 * waveform[2]), 128.0 * waveform[2], 1e-9));
    dither_filter_free(filter);
}

TEST_MAIN()
