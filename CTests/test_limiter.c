#include "../CTests/test_support.h"
#include "../CLib/Filters/limiter.h"
#include <math.h>

TEST(test_hard_clip) {
    prc_fmt_t waveform[] = {-2.0, -1.0, 0.0, 0.5, 1.5, 2.0};
    limiter_parameters_t params = {
        .clip_limit = -6.020599913279624, // -6.02 dB = 0.5 linear limit
        .soft_clip = false
    };
    limiter_filter_t* filter = limiter_filter_create("limiter", &params);
    ASSERT_TRUE(filter != NULL);
    
    limiter_filter_process(filter, waveform, 6);
    
    prc_fmt_t expected[] = {-0.5, -0.5, 0.0, 0.5, 0.5, 0.5};
    for (size_t i = 0; i < 6; i++) {
        ASSERT_NEAR(expected[i], waveform[i], 1e-5);
    }
    limiter_filter_free(filter);
}

TEST(test_soft_clip) {
    prc_fmt_t waveform[] = {-2.0, -0.5, 0.0, 0.5, 2.0};
    limiter_parameters_t params = {
        .clip_limit = 0.0, // 0 dB = 1.0 linear limit
        .soft_clip = true
    };
    limiter_filter_t* filter = limiter_filter_create("limiter", &params);
    ASSERT_TRUE(filter != NULL);
    
    limiter_filter_process(filter, waveform, 5);
    
    prc_fmt_t expected[] = {-1.0, -0.481481, 0.0, 0.481481, 1.0};
    for (size_t i = 0; i < 5; i++) {
        ASSERT_NEAR(expected[i], waveform[i], 1e-5);
    }
    limiter_filter_free(filter);
}

TEST_MAIN()
