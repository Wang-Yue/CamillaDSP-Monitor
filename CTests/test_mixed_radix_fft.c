#include "../CTests/test_support.h"
#include "../CLib/FFT/mixed_radix_fft.h"
#include <math.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct { uint64_t state; } splitmix64_t;

static uint64_t splitmix64_next(splitmix64_t* rng) {
    rng->state += 0x9E3779B97F4A7C15ULL;
    uint64_t z = rng->state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static double splitmix64_next_unit(splitmix64_t* rng) {
    return (double)(splitmix64_next(rng) >> 11) * (1.0 / 9007199254740992.0);
}

static void direct_dft(const double* real_in, const double* imag_in, double* real_out, double* imag_out, size_t n, bool inverse) {
    double sign = inverse ? 1.0 : -1.0;
    for (size_t k = 0; k < n; k++) {
        double sumR = 0.0;
        double sumI = 0.0;
        for (size_t nn = 0; nn < n; nn++) {
            double theta = sign * 2.0 * M_PI * (double)(nn * k) / (double)n;
            double cR = cos(theta);
            double cI = sin(theta);
            sumR += real_in[nn] * cR - imag_in[nn] * cI;
            sumI += real_in[nn] * cI + imag_in[nn] * cR;
        }
        real_out[k] = sumR;
        imag_out[k] = sumI;
    }
}

static double max_abs_diff(const double* re_a, const double* im_a, const double* re_b, const double* im_b, size_t n) {
    double max_diff = 0.0;
    for (size_t i = 0; i < n; i++) {
        double dr = fabs(re_a[i] - re_b[i]);
        double di = fabs(im_a[i] - im_b[i]);
        if (dr > max_diff) max_diff = dr;
        if (di > max_diff) max_diff = di;
    }
    return max_diff;
}

static void random_complex(double* re, double* im, size_t n, uint64_t seed) {
    splitmix64_t rng = { seed };
    for (size_t i = 0; i < n; i++) {
        re[i] = splitmix64_next_unit(&rng) * 2.0 - 1.0;
        im[i] = splitmix64_next_unit(&rng) * 2.0 - 1.0;
    }
}

static bool check_matches_direct_dft(size_t n) {
    double* in_re = (double*)malloc(n * sizeof(double));
    double* in_im = (double*)malloc(n * sizeof(double));
    double* mr_re = (double*)malloc(n * sizeof(double));
    double* mr_im = (double*)malloc(n * sizeof(double));
    double* dir_re = (double*)malloc(n * sizeof(double));
    double* dir_im = (double*)malloc(n * sizeof(double));

    random_complex(in_re, in_im, n, (uint64_t)n);

    mixed_radix_fft_t* fft = mixed_radix_fft_create(n);
    if (!fft) {
        free(in_re); free(in_im); free(mr_re); free(mr_im); free(dir_re); free(dir_im);
        return false;
    }
    mixed_radix_fft_execute(fft, in_re, in_im, mr_re, mr_im, false);
    direct_dft(in_re, in_im, dir_re, dir_im, n, false);

    double diff = max_abs_diff(mr_re, mr_im, dir_re, dir_im, n);
    double tol = 1e-10 * (double)n;

    mixed_radix_fft_free(fft);
    free(in_re); free(in_im); free(mr_re); free(mr_im); free(dir_re); free(dir_im);

    if (diff >= tol) {
        printf("Failed for n=%zu: diff=%g >= tol=%g\n", n, diff, tol);
        return false;
    }
    return true;
}

TEST(RadixIsolated) {
    size_t sizes[] = {2, 3, 4, 5, 7, 8, 9};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        ASSERT_TRUE(check_matches_direct_dft(sizes[i]));
    }
}

TEST(CompositeSizes) {
    size_t sizes[] = {6, 10, 12, 14, 15, 16, 21, 25, 35, 49, 64, 105, 147, 343, 1029, 1120};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        ASSERT_TRUE(check_matches_direct_dft(sizes[i]));
    }
}

TEST(RoundTrip) {
    size_t sizes[] = {3, 5, 7, 14, 21, 1029, 1120};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        size_t n = sizes[i];
        double* in_re = (double*)malloc(n * sizeof(double));
        double* in_im = (double*)malloc(n * sizeof(double));
        double* fwd_re = (double*)malloc(n * sizeof(double));
        double* fwd_im = (double*)malloc(n * sizeof(double));
        double* back_re = (double*)malloc(n * sizeof(double));
        double* back_im = (double*)malloc(n * sizeof(double));

        random_complex(in_re, in_im, n, (uint64_t)n * 0x9E3779B97F4A7C15ULL);

        mixed_radix_fft_t* fft = mixed_radix_fft_create(n);
        ASSERT_TRUE(fft != NULL);
        mixed_radix_fft_execute(fft, in_re, in_im, fwd_re, fwd_im, false);
        mixed_radix_fft_execute(fft, fwd_re, fwd_im, back_re, back_im, true);

        double scale = 1.0 / (double)n;
        double max_diff = 0.0;
        for (size_t k = 0; k < n; k++) {
            double dr = fabs(back_re[k] * scale - in_re[k]);
            double di = fabs(back_im[k] * scale - in_im[k]);
            if (dr > max_diff) max_diff = dr;
            if (di > max_diff) max_diff = di;
        }
        ASSERT_TRUE(max_diff < 5e-13);

        mixed_radix_fft_free(fft);
        free(in_re); free(in_im); free(fwd_re); free(fwd_im); free(back_re); free(back_im);
    }
}

TEST(InverseDirection) {
    size_t sizes[] = {5, 7, 21, 49, 1029};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        size_t n = sizes[i];
        double* in_re = (double*)malloc(n * sizeof(double));
        double* in_im = (double*)malloc(n * sizeof(double));
        double* mr_re = (double*)malloc(n * sizeof(double));
        double* mr_im = (double*)malloc(n * sizeof(double));
        double* dir_re = (double*)malloc(n * sizeof(double));
        double* dir_im = (double*)malloc(n * sizeof(double));

        random_complex(in_re, in_im, n, (uint64_t)n + 1);

        mixed_radix_fft_t* fft = mixed_radix_fft_create(n);
        ASSERT_TRUE(fft != NULL);
        mixed_radix_fft_execute(fft, in_re, in_im, mr_re, mr_im, true);
        direct_dft(in_re, in_im, dir_re, dir_im, n, true);

        double diff = max_abs_diff(mr_re, mr_im, dir_re, dir_im, n);
        double tol = 1e-10 * (double)n;
        ASSERT_TRUE(diff < tol);

        mixed_radix_fft_free(fft);
        free(in_re); free(in_im); free(mr_re); free(mr_im); free(dir_re); free(dir_im);
    }
}

TEST(UnsupportedFactorsReturnNil) {
    size_t sizes[] = {11, 13, 17, 22, 33, 121};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        mixed_radix_fft_t* fft = mixed_radix_fft_create(sizes[i]);
        ASSERT_TRUE(fft == NULL);
    }
}

TEST_MAIN()
