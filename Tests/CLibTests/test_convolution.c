#include "test_support.h"
#include "../../Sources/CDSP/Filters/convolution.h"
#include <math.h>


TEST(MovingAverage) {
    double coeffs[] = {0.5, 0.5};
    conv_parameters_t params = { .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 2 };
    convolution_filter_t* filter = convolution_filter_create("conv", &params, 8);
    ASSERT_TRUE(filter != NULL);

    double wave[] = {1.0, 1.0, 1.0, 0.0, 0.0, -1.0, 0.0, 0.0};
    double expected[] = {0.5, 1.0, 1.0, 0.5, 0.0, -0.5, -0.5, 0.0};

    convolution_filter_process(filter, wave, 8);
    for (size_t i = 0; i < 8; i++) {
        ASSERT_NEAR(expected[i], wave[i], 1e-7);
    }
    convolution_filter_free(filter);
}

TEST(SegmentedConvolution) {
    double ir[32];
    for (int i = 0; i < 32; i++) ir[i] = (double)i;
    conv_parameters_t params = { .type = CONV_TYPE_VALUES, .values = ir, .values_count = 32 };
    convolution_filter_t* filter = convolution_filter_create("conv", &params, 8);
    ASSERT_TRUE(filter != NULL);

    double impulse[8] = {1.0, 0, 0, 0, 0, 0, 0, 0};
    convolution_filter_process(filter, impulse, 8);
    for (int i = 0; i < 8; i++) ASSERT_NEAR((double)i, impulse[i], 1e-5);

    double zeros[8] = {0};
    convolution_filter_process(filter, zeros, 8);
    for (int i = 0; i < 8; i++) ASSERT_NEAR((double)(i + 8), zeros[i], 1e-5);

    memset(zeros, 0, sizeof(zeros));
    convolution_filter_process(filter, zeros, 8);
    for (int i = 0; i < 8; i++) ASSERT_NEAR((double)(i + 16), zeros[i], 1e-5);

    memset(zeros, 0, sizeof(zeros));
    convolution_filter_process(filter, zeros, 8);
    for (int i = 0; i < 8; i++) ASSERT_NEAR((double)(i + 24), zeros[i], 1e-5);

    memset(zeros, 0, sizeof(zeros));
    convolution_filter_process(filter, zeros, 8);
    for (int i = 0; i < 8; i++) ASSERT_NEAR(0.0, zeros[i], 1e-5);

    convolution_filter_free(filter);
}

TEST(IdentityConvolution) {
    double coeffs[] = {1.0};
    conv_parameters_t params = { .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 1 };
    convolution_filter_t* filter = convolution_filter_create("conv", &params, 8);
    ASSERT_TRUE(filter != NULL);

    double wave[] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    convolution_filter_process(filter, wave, 8);
    ASSERT_NEAR(1.0, wave[0], 1e-7);
    for (int i = 1; i < 8; i++) ASSERT_NEAR(0.0, wave[i], 1e-7);
    convolution_filter_free(filter);
}

TEST(DelayConvolution) {
    double coeffs[] = {0.0, 0.0, 0.0, 1.0};
    conv_parameters_t params = { .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 4 };
    convolution_filter_t* filter = convolution_filter_create("conv", &params, 8);
    ASSERT_TRUE(filter != NULL);

    double wave[] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    convolution_filter_process(filter, wave, 8);
    ASSERT_NEAR(0.0, wave[0], 1e-7);
    ASSERT_NEAR(0.0, wave[1], 1e-7);
    ASSERT_NEAR(0.0, wave[2], 1e-7);
    ASSERT_NEAR(1.0, wave[3], 1e-7);
    for (int i = 4; i < 8; i++) ASSERT_NEAR(0.0, wave[i], 1e-7);
    convolution_filter_free(filter);
}

TEST(ConvolutionWithSineWave) {
    double coeffs[] = {0.5, 0.5};
    conv_parameters_t params = { .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 2 };
    convolution_filter_t* filter = convolution_filter_create("conv", &params, 64);
    ASSERT_TRUE(filter != NULL);

    double sample_rate = 48000.0;
    double freq = 100.0;
    double theta = 2.0 * M_PI * freq / sample_rate;
    double expected_gain = 0.5 * (1.0 + cos(theta));

    double wave[64];
    for (int chunk = 0; chunk < 8; chunk++) {
        int offset = chunk * 64;
        for (int i = 0; i < 64; i++) {
            wave[i] = cos(2.0 * M_PI * freq * (double)(offset + i) / sample_rate);
        }
        convolution_filter_process(filter, wave, 64);
    }

    double peak = 0.0;
    for (int i = 0; i < 64; i++) {
        if (fabs(wave[i]) > peak) peak = fabs(wave[i]);
    }
    ASSERT_TRUE(fabs(peak - expected_gain) < expected_gain * 0.10);
    convolution_filter_free(filter);
}

TEST(EmptyIRThrows) {
    conv_parameters_t params = { .type = CONV_TYPE_VALUES, .values = NULL, .values_count = 0 };
    convolution_filter_t* filter = convolution_filter_create("conv", &params, 8);
    ASSERT_TRUE(filter == NULL);
}

TEST(DummyIsIdentity) {
    conv_parameters_t params = { .type = CONV_TYPE_DUMMY, .length = 4 };
    convolution_filter_t* filter = convolution_filter_create("conv", &params, 8);
    ASSERT_TRUE(filter != NULL);

    double wave[] = {0.3, -0.2, 0.7, -0.1, 0.0, 0.5, -0.4, 0.9};
    double original[] = {0.3, -0.2, 0.7, -0.1, 0.0, 0.5, -0.4, 0.9};
    convolution_filter_process(filter, wave, 8);

    for (int i = 0; i < 8; i++) {
        ASSERT_NEAR(original[i], wave[i], 1e-7);
    }
    convolution_filter_free(filter);
}

TEST_MAIN()
