#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "../../Sources/CDSP/FFT/bluestein_fft.h"
#include "../../Sources/CDSP/FFT/real_fft.h"
#include "test_support.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
  uint64_t state;
} simple_splitmix_t;

static uint64_t simple_splitmix_next(simple_splitmix_t* rng) {
  rng->state += 0x9E3779B97F4A7C15ULL;
  uint64_t z = rng->state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

static double simple_splitmix_next_unit(simple_splitmix_t* rng) {
  return (double)(simple_splitmix_next(rng) >> 11) * (1.0 / 9007199254740992.0);
}

static void direct_dft(const double* real_in, const double* imag_in,
                       double* real_out, double* imag_out, size_t n,
                       bool inverse) {
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

static double max_abs_diff(const double* re_a, const double* im_a,
                           const double* re_b, const double* im_b, size_t n) {
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
  simple_splitmix_t rng = {seed};
  for (size_t i = 0; i < n; i++) {
    re[i] = simple_splitmix_next_unit(&rng) * 2.0 - 1.0;
    im[i] = simple_splitmix_next_unit(&rng) * 2.0 - 1.0;
  }
}

#if defined(ENABLE_ACCELERATE)
TEST(ForwardMatchesDirectDFT) {
  size_t sizes[] = {3, 7, 11, 13, 17, 19, 23, 29, 121, 169};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    size_t n = sizes[i];
    double* in_re = (double*)malloc(n * sizeof(double));
    double* in_im = (double*)malloc(n * sizeof(double));
    double* bs_re = (double*)malloc(n * sizeof(double));
    double* bs_im = (double*)malloc(n * sizeof(double));
    double* dir_re = (double*)malloc(n * sizeof(double));
    double* dir_im = (double*)malloc(n * sizeof(double));

    random_complex(in_re, in_im, n, (uint64_t)n * 0x9E3779B9ULL);

    bluestein_fft_t* fft = bluestein_fft_create(n);
    ASSERT_TRUE(fft != NULL);
    bluestein_fft_execute(fft, in_re, in_im, bs_re, bs_im, false);
    direct_dft(in_re, in_im, dir_re, dir_im, n, false);

    double diff = max_abs_diff(bs_re, bs_im, dir_re, dir_im, n);
    double tol = 1e-9 * (double)n;
    ASSERT_TRUE(diff < tol);

    bluestein_fft_free(fft);
    free(in_re);
    free(in_im);
    free(bs_re);
    free(bs_im);
    free(dir_re);
    free(dir_im);
  }
}

TEST(InverseMatchesDirectDFT) {
  size_t sizes[] = {11, 13, 17, 23, 121};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    size_t n = sizes[i];
    double* in_re = (double*)malloc(n * sizeof(double));
    double* in_im = (double*)malloc(n * sizeof(double));
    double* bs_re = (double*)malloc(n * sizeof(double));
    double* bs_im = (double*)malloc(n * sizeof(double));
    double* dir_re = (double*)malloc(n * sizeof(double));
    double* dir_im = (double*)malloc(n * sizeof(double));

    random_complex(in_re, in_im, n, (uint64_t)n);

    bluestein_fft_t* fft = bluestein_fft_create(n);
    ASSERT_TRUE(fft != NULL);
    bluestein_fft_execute(fft, in_re, in_im, bs_re, bs_im, true);
    direct_dft(in_re, in_im, dir_re, dir_im, n, true);

    double diff = max_abs_diff(bs_re, bs_im, dir_re, dir_im, n);
    double tol = 1e-9 * (double)n;
    ASSERT_TRUE(diff < tol);

    bluestein_fft_free(fft);
    free(in_re);
    free(in_im);
    free(bs_re);
    free(bs_im);
    free(dir_re);
    free(dir_im);
  }
}

TEST(RoundTrip) {
  size_t sizes[] = {11, 13, 17, 22, 121, 169};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    size_t n = sizes[i];
    double* in_re = (double*)malloc(n * sizeof(double));
    double* in_im = (double*)malloc(n * sizeof(double));
    double* fwd_re = (double*)malloc(n * sizeof(double));
    double* fwd_im = (double*)malloc(n * sizeof(double));
    double* back_re = (double*)malloc(n * sizeof(double));
    double* back_im = (double*)malloc(n * sizeof(double));

    random_complex(in_re, in_im, n, (uint64_t)n + 7);

    bluestein_fft_t* fft = bluestein_fft_create(n);
    ASSERT_TRUE(fft != NULL);
    bluestein_fft_execute(fft, in_re, in_im, fwd_re, fwd_im, false);
    bluestein_fft_execute(fft, fwd_re, fwd_im, back_re, back_im, true);

    double scale = 1.0 / (double)n;
    double max_diff = 0.0;
    for (size_t k = 0; k < n; k++) {
      double dr = fabs(back_re[k] * scale - in_re[k]);
      double di = fabs(back_im[k] * scale - in_im[k]);
      if (dr > max_diff) max_diff = dr;
      if (di > max_diff) max_diff = di;
    }
    ASSERT_TRUE(max_diff < 5e-13);

    bluestein_fft_free(fft);
    free(in_re);
    free(in_im);
    free(fwd_re);
    free(fwd_im);
    free(back_re);
    free(back_im);
  }
}
#endif  // ENABLE_ACCELERATE

TEST(RealFFTFallbackForPrimeFactors) {
  size_t length = 22;
  real_fft_t* real_fft = real_fft_create(length);
  ASSERT_TRUE(real_fft != NULL);
  ASSERT_EQ(length / 2 + 1, real_fft->spectrum_length);

  double* input = (double*)calloc(length, sizeof(double));
  input[0] = 1.0;
  double* spec_re = (double*)calloc(real_fft->spectrum_length, sizeof(double));
  double* spec_im = (double*)calloc(real_fft->spectrum_length, sizeof(double));

  real_fft_forward(real_fft, input, spec_re, spec_im);
  for (size_t k = 0; k < real_fft->spectrum_length; k++) {
    double mag = sqrt(spec_re[k] * spec_re[k] + spec_im[k] * spec_im[k]);
    ASSERT_NEAR(1.0, mag, 1e-12);
  }

  double* recovered = (double*)calloc(length, sizeof(double));
  real_fft_inverse(real_fft, spec_re, spec_im, recovered);
  ASSERT_NEAR((double)length, recovered[0], 1e-10);
  for (size_t k = 1; k < length; k++) {
    ASSERT_NEAR(0.0, recovered[k], 1e-10);
  }

  real_fft_free(real_fft);
  free(input);
  free(spec_re);
  free(spec_im);
  free(recovered);
}

TEST(RealFFTVDSPDFTInnerRoundtrip) {
  size_t lengths[] = {48, 80, 240, 2560};
  for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
    size_t length = lengths[i];
    real_fft_t* real_fft = real_fft_create(length);
    ASSERT_TRUE(real_fft != NULL);
    ASSERT_EQ(length / 2 + 1, real_fft->spectrum_length);

    double* input = (double*)calloc(length, sizeof(double));
    input[0] = 1.0;
    double* spec_re =
        (double*)calloc(real_fft->spectrum_length, sizeof(double));
    double* spec_im =
        (double*)calloc(real_fft->spectrum_length, sizeof(double));

    real_fft_forward(real_fft, input, spec_re, spec_im);
    for (size_t k = 0; k < real_fft->spectrum_length; k++) {
      double mag = sqrt(spec_re[k] * spec_re[k] + spec_im[k] * spec_im[k]);
      ASSERT_NEAR(1.0, mag, 1e-12);
    }

    double* recovered = (double*)calloc(length, sizeof(double));
    real_fft_inverse(real_fft, spec_re, spec_im, recovered);
    ASSERT_NEAR((double)length, recovered[0], 1e-9);
    for (size_t k = 1; k < length; k++) {
      ASSERT_NEAR(0.0, recovered[k], 1e-9);
    }

    real_fft_free(real_fft);
    free(input);
    free(spec_re);
    free(spec_im);
    free(recovered);
  }
}

TEST(RealFFTPow2VDSPRoundtrip) {
  size_t lengths[] = {8, 16, 32, 64, 1024, 2048, 4096};
  for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
    size_t length = lengths[i];
    real_fft_t* real_fft = real_fft_create(length);
    ASSERT_TRUE(real_fft != NULL);
    ASSERT_EQ(length / 2 + 1, real_fft->spectrum_length);

    double* input = (double*)calloc(length, sizeof(double));
    input[0] = 1.0;
    double* spec_re =
        (double*)calloc(real_fft->spectrum_length, sizeof(double));
    double* spec_im =
        (double*)calloc(real_fft->spectrum_length, sizeof(double));

    real_fft_forward(real_fft, input, spec_re, spec_im);
    for (size_t k = 0; k < real_fft->spectrum_length; k++) {
      double mag = sqrt(spec_re[k] * spec_re[k] + spec_im[k] * spec_im[k]);
      ASSERT_NEAR(1.0, mag, 1e-12);
    }
    ASSERT_DOUBLE_EQ(0.0, spec_im[0]);
    ASSERT_DOUBLE_EQ(0.0, spec_im[real_fft->spectrum_length - 1]);

    double* recovered = (double*)calloc(length, sizeof(double));
    real_fft_inverse(real_fft, spec_re, spec_im, recovered);
    ASSERT_NEAR((double)length, recovered[0], 1e-10);
    for (size_t k = 1; k < length; k++) {
      ASSERT_NEAR(0.0, recovered[k], 1e-10);
    }

    size_t k_bin = length / 4;
    if (k_bin < 1) k_bin = 1;
    for (size_t n = 0; n < length; n++) {
      input[n] = cos(2.0 * M_PI * (double)k_bin * (double)n / (double)length);
    }
    real_fft_forward(real_fft, input, spec_re, spec_im);
    double expected_re = (double)length / 2.0;
    ASSERT_NEAR(expected_re, spec_re[k_bin], 1e-9);
    ASSERT_NEAR(0.0, spec_im[k_bin], 1e-9);

    real_fft_free(real_fft);
    free(input);
    free(spec_re);
    free(spec_im);
    free(recovered);
  }
}

TEST_MAIN()
