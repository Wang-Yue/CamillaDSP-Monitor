#include "../CTests/test_support.h"
#include "../CLib/Filters/diffeq.h"
#include <math.h>

static bool is_close(prc_fmt_t left, prc_fmt_t right, prc_fmt_t maxdiff) {
    return fabs(left - right) < maxdiff;
}

static bool compare_waveforms(const prc_fmt_t* left, const prc_fmt_t* right, size_t count, prc_fmt_t maxdiff) {
    for (size_t i = 0; i < count; i++) {
        if (!is_close(left[i], right[i], maxdiff)) return false;
    }
    return true;
}

TEST(check_result) {
    double a_vals[] = {1.0, -0.1462978543780541, 0.005350765548905586};
    double b_vals[] = {0.21476322779271284, 0.4295264555854257, 0.21476322779271284};
    diff_eq_parameters_t params = {
        .a = a_vals, .a_count = 3,
        .b = b_vals, .b_count = 3
    };
    diffeq_filter_t* filter = diffeq_filter_create("diffeq", &params);
    ASSERT_TRUE(filter != NULL);

    prc_fmt_t wave[] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    prc_fmt_t expected[] = {0.215, 0.461, 0.281, 0.039, 0.004, 0.0, 0.0, 0.0};

    diffeq_filter_process(filter, wave, 8);

    ASSERT_TRUE(compare_waveforms(wave, expected, 8, 1e-3));
    diffeq_filter_free(filter);
}

TEST_MAIN()
