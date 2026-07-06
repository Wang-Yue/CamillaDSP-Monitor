#include <math.h>

#include "../../Sources/CDSP/Filters/biquad.h"
#include "test_support.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void gain_and_phase(const biquad_coefficients_t* coeffs, double f,
                           double fs, double* gain_db, double* phase_deg) {
  *gain_db = biquad_coefficients_gain_db(coeffs, f, (int)fs);
  *phase_deg = biquad_coefficients_phase_rad(coeffs, f, (int)fs) * 180.0 / M_PI;
}

static bool is_close(double left, double right, double maxdiff) {
  return fabs(left - right) < maxdiff;
}

static bool is_close_relative(double left, double right, double maxdiff) {
  return fabs(left / right - 1.0) < maxdiff;
}

static biquad_coefficients_t make_coeffs(biquad_parameters_t* params,
                                         int sample_rate) {
  biquad_coefficients_t coeffs = {0};
  biquad_coefficients_compute(params, sample_rate, &coeffs);
  return coeffs;
}

TEST(ImpulseResponse) {
  biquad_parameters_t params = {
      .type = BIQUAD_TYPE_LOWPASS, .freq = 10000.0, .q = 0.5};
  biquad_coefficients_t coeffs = make_coeffs(&params, 44100);
  biquad_filter_t* filter = biquad_filter_create("biquad", &coeffs);
  ASSERT_TRUE(filter != NULL);

  double wave[] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  double expected[] = {0.215, 0.461, 0.281, 0.039, 0.004, 0.0, 0.0, 0.0};

  biquad_filter_process(filter, wave, 8);

  for (size_t i = 0; i < 8; i++) {
    ASSERT_TRUE(is_close(wave[i], expected[i], 1e-3));
  }
  biquad_filter_free(filter);
}

TEST(Lowpass) {
  biquad_parameters_t params = {
      .type = BIQUAD_TYPE_LOWPASS, .freq = 100.0, .q = 1.0 / sqrt(2.0)};
  biquad_coefficients_t coeffs = make_coeffs(&params, 44100);
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&coeffs, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&coeffs, 400.0, 44100.0, &ghf, &phf);
  gain_and_phase(&coeffs, 10.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, -3.0, 0.1));
  ASSERT_TRUE(is_close(glf, 0.0, 0.1));
  ASSERT_TRUE(is_close(ghf, -24.0, 0.2));
}

TEST(Highpass) {
  biquad_parameters_t params = {
      .type = BIQUAD_TYPE_HIGHPASS, .freq = 100.0, .q = 1.0 / sqrt(2.0)};
  biquad_coefficients_t coeffs = make_coeffs(&params, 44100);
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&coeffs, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&coeffs, 400.0, 44100.0, &ghf, &phf);
  gain_and_phase(&coeffs, 25.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, -3.0, 0.1));
  ASSERT_TRUE(is_close(glf, -24.0, 0.2));
  ASSERT_TRUE(is_close(ghf, 0.0, 0.1));
}

TEST(LowpassFO) {
  biquad_parameters_t params = {.type = BIQUAD_TYPE_LOWPASS_FO, .freq = 100.0};
  biquad_coefficients_t coeffs = make_coeffs(&params, 44100);
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&coeffs, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&coeffs, 400.0, 44100.0, &ghf, &phf);
  gain_and_phase(&coeffs, 10.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, -3.0, 0.1));
  ASSERT_TRUE(is_close(glf, 0.0, 0.1));
  ASSERT_TRUE(is_close(ghf, -12.3, 0.1));
}

TEST(HighpassFO) {
  biquad_parameters_t params = {.type = BIQUAD_TYPE_HIGHPASS_FO, .freq = 100.0};
  biquad_coefficients_t coeffs = make_coeffs(&params, 44100);
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&coeffs, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&coeffs, 800.0, 44100.0, &ghf, &phf);
  gain_and_phase(&coeffs, 25.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, -3.0, 0.1));
  ASSERT_TRUE(is_close(glf, -12.3, 0.1));
  ASSERT_TRUE(is_close(ghf, 0.0, 0.1));
}

TEST(Peaking) {
  biquad_parameters_t params = {
      .type = BIQUAD_TYPE_PEAKING, .freq = 100.0, .q = 3.0, .gain = 7.0};
  biquad_coefficients_t coeffs = make_coeffs(&params, 44100);
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&coeffs, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&coeffs, 400.0, 44100.0, &ghf, &phf);
  gain_and_phase(&coeffs, 25.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, 7.0, 0.1));
  ASSERT_TRUE(is_close(glf, 0.0, 0.1));
  ASSERT_TRUE(is_close(ghf, 0.0, 0.1));
}

TEST(Bandpass) {
  biquad_parameters_t params = {
      .type = BIQUAD_TYPE_BANDPASS, .freq = 100.0, .q = 1.0};
  biquad_coefficients_t coeffs = make_coeffs(&params, 44100);
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&coeffs, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&coeffs, 400.0, 44100.0, &ghf, &phf);
  gain_and_phase(&coeffs, 25.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, 0.0, 0.1));
  ASSERT_TRUE(is_close(glf, -12.0, 0.3));
  ASSERT_TRUE(is_close(ghf, -12.0, 0.3));
}

TEST(Notch) {
  biquad_parameters_t params = {
      .type = BIQUAD_TYPE_NOTCH, .freq = 100.0, .q = 3.0};
  biquad_coefficients_t coeffs = make_coeffs(&params, 44100);
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&coeffs, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&coeffs, 400.0, 44100.0, &ghf, &phf);
  gain_and_phase(&coeffs, 25.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(gf0 < -40.0);
  ASSERT_TRUE(is_close(glf, 0.0, 0.1));
  ASSERT_TRUE(is_close(ghf, 0.0, 0.1));
}

TEST(Allpass) {
  biquad_parameters_t params = {
      .type = BIQUAD_TYPE_ALLPASS, .freq = 100.0, .q = 3.0};
  biquad_coefficients_t coeffs = make_coeffs(&params, 44100);
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&coeffs, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&coeffs, 10000.0, 44100.0, &ghf, &phf);
  gain_and_phase(&coeffs, 1.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, 0.0, 0.1));
  ASSERT_TRUE(is_close(glf, 0.0, 0.1));
  ASSERT_TRUE(is_close(ghf, 0.0, 0.1));
  ASSERT_TRUE(is_close(fabs(pf0), 180.0, 0.5));
  ASSERT_TRUE(is_close(plf, 0.0, 0.5));
  ASSERT_TRUE(is_close(phf, 0.0, 0.5));
}

TEST(AllpassFO) {
  biquad_parameters_t params = {.type = BIQUAD_TYPE_ALLPASS_FO, .freq = 100.0};
  biquad_coefficients_t coeffs = make_coeffs(&params, 44100);
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&coeffs, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&coeffs, 10000.0, 44100.0, &ghf, &phf);
  gain_and_phase(&coeffs, 1.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, 0.0, 0.1));
  ASSERT_TRUE(is_close(glf, 0.0, 0.1));
  ASSERT_TRUE(is_close(ghf, 0.0, 0.1));
  ASSERT_TRUE(is_close(fabs(pf0), 90.0, 0.5));
  ASSERT_TRUE(is_close(plf, 0.0, 2.0));
  ASSERT_TRUE(is_close(fabs(phf), 180.0, 2.0));
}

TEST(Highshelf) {
  biquad_parameters_t params = {.type = BIQUAD_TYPE_HIGHSHELF,
                                .freq = 100.0,
                                .gain = -24.0,
                                .slope = 6.0};
  biquad_coefficients_t coeffs = make_coeffs(&params, 44100);
  double gf0, pf0, gf0h, pf0h, gf0l, pf0l, ghf, phf, glf, plf;
  gain_and_phase(&coeffs, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&coeffs, 200.0, 44100.0, &gf0h, &pf0h);
  gain_and_phase(&coeffs, 50.0, 44100.0, &gf0l, &pf0l);
  gain_and_phase(&coeffs, 10000.0, 44100.0, &ghf, &phf);
  gain_and_phase(&coeffs, 1.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, -12.0, 0.1));
  ASSERT_TRUE(is_close(gf0h, -18.0, 1.0));
  ASSERT_TRUE(is_close(gf0l, -6.0, 1.0));
  ASSERT_TRUE(is_close(glf, 0.0, 0.1));
  ASSERT_TRUE(is_close(ghf, -24.0, 0.1));
}

TEST(Lowshelf) {
  biquad_parameters_t params = {
      .type = BIQUAD_TYPE_LOWSHELF, .freq = 100.0, .gain = -24.0, .slope = 6.0};
  biquad_coefficients_t coeffs = make_coeffs(&params, 44100);
  double gf0, pf0, gf0h, pf0h, gf0l, pf0l, ghf, phf, glf, plf;
  gain_and_phase(&coeffs, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&coeffs, 200.0, 44100.0, &gf0h, &pf0h);
  gain_and_phase(&coeffs, 50.0, 44100.0, &gf0l, &pf0l);
  gain_and_phase(&coeffs, 10000.0, 44100.0, &ghf, &phf);
  gain_and_phase(&coeffs, 1.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, -12.0, 0.1));
  ASSERT_TRUE(is_close(gf0h, -6.0, 1.0));
  ASSERT_TRUE(is_close(gf0l, -18.0, 1.0));
  ASSERT_TRUE(is_close(glf, -24.0, 0.1));
  ASSERT_TRUE(is_close(ghf, 0.0, 0.1));
}

TEST(LowshelfSlopeVsQ) {
  biquad_parameters_t pS = {.type = BIQUAD_TYPE_LOWSHELF,
                            .freq = 100.0,
                            .gain = -24.0,
                            .slope = 12.0};
  biquad_parameters_t pQ = {.type = BIQUAD_TYPE_LOWSHELF,
                            .freq = 100.0,
                            .q = 1.0 / sqrt(2.0),
                            .gain = -24.0};
  biquad_coefficients_t cS = make_coeffs(&pS, 44100);
  biquad_coefficients_t cQ = make_coeffs(&pQ, 44100);
  ASSERT_TRUE(is_close_relative(cS.a1, cQ.a1, 0.001));
  ASSERT_TRUE(is_close_relative(cS.a2, cQ.a2, 0.001));
  ASSERT_TRUE(is_close_relative(cS.b0, cQ.b0, 0.001));
  ASSERT_TRUE(is_close_relative(cS.b1, cQ.b1, 0.001));
  ASSERT_TRUE(is_close_relative(cS.b2, cQ.b2, 0.001));
}

TEST(HighshelfSlopeVsQ) {
  biquad_parameters_t pS = {.type = BIQUAD_TYPE_HIGHSHELF,
                            .freq = 100.0,
                            .gain = -24.0,
                            .slope = 12.0};
  biquad_parameters_t pQ = {.type = BIQUAD_TYPE_HIGHSHELF,
                            .freq = 100.0,
                            .q = 1.0 / sqrt(2.0),
                            .gain = -24.0};
  biquad_coefficients_t cS = make_coeffs(&pS, 44100);
  biquad_coefficients_t cQ = make_coeffs(&pQ, 44100);
  ASSERT_TRUE(is_close_relative(cS.a1, cQ.a1, 0.001));
  ASSERT_TRUE(is_close_relative(cS.a2, cQ.a2, 0.001));
  ASSERT_TRUE(is_close_relative(cS.b0, cQ.b0, 0.001));
  ASSERT_TRUE(is_close_relative(cS.b1, cQ.b1, 0.001));
  ASSERT_TRUE(is_close_relative(cS.b2, cQ.b2, 0.001));
}

TEST(BandpassBWvsQ) {
  biquad_parameters_t pBW = {
      .type = BIQUAD_TYPE_BANDPASS, .freq = 100.0, .bandwidth = 1.0};
  biquad_parameters_t pQ = {
      .type = BIQUAD_TYPE_BANDPASS, .freq = 100.0, .q = sqrt(2.0)};
  biquad_coefficients_t cBW = make_coeffs(&pBW, 44100);
  biquad_coefficients_t cQ = make_coeffs(&pQ, 44100);
  ASSERT_TRUE(is_close_relative(cBW.a1, cQ.a1, 0.001));
  ASSERT_TRUE(is_close_relative(cBW.a2, cQ.a2, 0.001));
  ASSERT_TRUE(is_close_relative(cBW.b0, cQ.b0, 0.001));
  ASSERT_TRUE(cBW.b1 == 0.0 && cQ.b1 == 0.0);
  ASSERT_TRUE(is_close_relative(cBW.b2, cQ.b2, 0.001));
}

TEST(NotchBWvsQ) {
  biquad_parameters_t pBW = {
      .type = BIQUAD_TYPE_NOTCH, .freq = 100.0, .bandwidth = 1.0};
  biquad_parameters_t pQ = {
      .type = BIQUAD_TYPE_NOTCH, .freq = 100.0, .q = sqrt(2.0)};
  biquad_coefficients_t cBW = make_coeffs(&pBW, 44100);
  biquad_coefficients_t cQ = make_coeffs(&pQ, 44100);
  ASSERT_TRUE(is_close_relative(cBW.a1, cQ.a1, 0.001));
  ASSERT_TRUE(is_close_relative(cBW.a2, cQ.a2, 0.001));
  ASSERT_TRUE(is_close_relative(cBW.b0, cQ.b0, 0.001));
  ASSERT_TRUE(is_close_relative(cBW.b1, cQ.b1, 0.001));
  ASSERT_TRUE(is_close_relative(cBW.b2, cQ.b2, 0.001));
}

TEST(AllpassBWvsQ) {
  biquad_parameters_t pBW = {
      .type = BIQUAD_TYPE_ALLPASS, .freq = 100.0, .bandwidth = 1.0};
  biquad_parameters_t pQ = {
      .type = BIQUAD_TYPE_ALLPASS, .freq = 100.0, .q = sqrt(2.0)};
  biquad_coefficients_t cBW = make_coeffs(&pBW, 44100);
  biquad_coefficients_t cQ = make_coeffs(&pQ, 44100);
  ASSERT_TRUE(is_close_relative(cBW.a1, cQ.a1, 0.001));
  ASSERT_TRUE(is_close_relative(cBW.a2, cQ.a2, 0.001));
  ASSERT_TRUE(is_close_relative(cBW.b0, cQ.b0, 0.001));
  ASSERT_TRUE(is_close_relative(cBW.b1, cQ.b1, 0.001));
  ASSERT_TRUE(is_close_relative(cBW.b2, cQ.b2, 0.001));
}

TEST(HighshelfFO) {
  biquad_parameters_t params = {
      .type = BIQUAD_TYPE_HIGHSHELF_FO, .freq = 100.0, .gain = -12.0};
  biquad_coefficients_t coeffs = make_coeffs(&params, 44100);
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&coeffs, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&coeffs, 10000.0, 44100.0, &ghf, &phf);
  gain_and_phase(&coeffs, 1.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, -6.0, 0.1));
  ASSERT_TRUE(is_close(glf, 0.0, 0.1));
  ASSERT_TRUE(is_close(ghf, -12.0, 0.1));
}

TEST(LowshelfFO) {
  biquad_parameters_t params = {
      .type = BIQUAD_TYPE_LOWSHELF_FO, .freq = 100.0, .gain = -12.0};
  biquad_coefficients_t coeffs = make_coeffs(&params, 44100);
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&coeffs, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&coeffs, 10000.0, 44100.0, &ghf, &phf);
  gain_and_phase(&coeffs, 1.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, -6.0, 0.1));
  ASSERT_TRUE(is_close(glf, -12.0, 0.1));
  ASSERT_TRUE(is_close(ghf, 0.0, 0.1));
}

TEST(FreeBiquad) {
  biquad_parameters_t params = {.type = BIQUAD_TYPE_FREE,
                                .a1 = -0.5,
                                .a2 = 0.1,
                                .b0 = 0.25,
                                .b1 = 0.5,
                                .b2 = 0.25};
  biquad_coefficients_t coeffs = make_coeffs(&params, 44100);
  ASSERT_DOUBLE_EQ(0.25, coeffs.b0);
  ASSERT_DOUBLE_EQ(0.5, coeffs.b1);
  ASSERT_DOUBLE_EQ(0.25, coeffs.b2);
  ASSERT_DOUBLE_EQ(-0.5, coeffs.a1);
  ASSERT_DOUBLE_EQ(0.1, coeffs.a2);
}

TEST(GeneralNotchHP) {
  biquad_parameters_t params = {.type = BIQUAD_TYPE_GENERAL_NOTCH,
                                .q = 1.0,
                                .freq_notch = 1000.0,
                                .freq_pole = 2000.0,
                                .normalize_at_dc = false};
  biquad_coefficients_t coeffs = make_coeffs(&params, 44100);
  double gain_fp, p1, gain_hf, p2, gain_lf, p3;
  gain_and_phase(&coeffs, 1000.0, 44100.0, &gain_fp, &p1);
  gain_and_phase(&coeffs, 20000.0, 44100.0, &gain_hf, &p2);
  gain_and_phase(&coeffs, 1.0, 44100.0, &gain_lf, &p3);
  ASSERT_TRUE(gain_fp < -40.0);
  ASSERT_TRUE(is_close(gain_lf, -12.1, 0.1));
  ASSERT_TRUE(is_close(gain_hf, 0.0, 0.1));
}

TEST(GeneralNotchLP) {
  biquad_parameters_t params = {.type = BIQUAD_TYPE_GENERAL_NOTCH,
                                .q = 1.0,
                                .freq_notch = 1000.0,
                                .freq_pole = 500.0,
                                .normalize_at_dc = true};
  biquad_coefficients_t coeffs = make_coeffs(&params, 44100);
  double gain_fp, p1, gain_hf, p2, gain_lf, p3;
  gain_and_phase(&coeffs, 1000.0, 44100.0, &gain_fp, &p1);
  gain_and_phase(&coeffs, 20000.0, 44100.0, &gain_hf, &p2);
  gain_and_phase(&coeffs, 1.0, 44100.0, &gain_lf, &p3);
  ASSERT_TRUE(gain_fp < -40.0);
  ASSERT_TRUE(is_close(gain_lf, 0.0, 0.1));
  ASSERT_TRUE(is_close(gain_hf, -12.1, 0.1));
}

TEST(LinkwitzTransform) {
  biquad_parameters_t params = {.type = BIQUAD_TYPE_LINKWITZ_TRANSFORM,
                                .freq_act = 100.0,
                                .q_act = 1.2,
                                .freq_target = 25.0,
                                .q_target = 0.7};
  biquad_coefficients_t coeffs = make_coeffs(&params, 44100);
  double gain10, p1, gain87, p2, gain123, p3, gain_hf, p4;
  gain_and_phase(&coeffs, 10.0, 44100.0, &gain10, &p1);
  gain_and_phase(&coeffs, 87.0, 44100.0, &gain87, &p2);
  gain_and_phase(&coeffs, 123.0, 44100.0, &gain123, &p3);
  gain_and_phase(&coeffs, 10000.0, 44100.0, &gain_hf, &p4);
  ASSERT_TRUE(is_close(gain10, 23.9, 0.1));
  ASSERT_TRUE(is_close(gain87, 0.0, 0.1));
  ASSERT_TRUE(is_close(gain123, -2.4, 0.1));
  ASSERT_TRUE(is_close(gain_hf, 0.0, 0.1));
}

TEST(ValidateFreqQ) {
  int fs48 = 48000;
  biquad_parameters_t p1 = {
      .type = BIQUAD_TYPE_PEAKING, .freq = 1000.0, .q = 2.0, .gain = 1.23};
  ASSERT_EQ(0, biquad_parameters_validate(&p1, fs48, NULL));
  biquad_parameters_t p2 = {
      .type = BIQUAD_TYPE_PEAKING, .freq = 1000.0, .q = 0.0, .gain = 1.23};
  ASSERT_NE(0, biquad_parameters_validate(&p2, fs48, NULL));
  biquad_parameters_t p3 = {
      .type = BIQUAD_TYPE_PEAKING, .freq = 25000.0, .q = 1.0, .gain = 1.23};
  ASSERT_NE(0, biquad_parameters_validate(&p3, fs48, NULL));
  biquad_parameters_t p4 = {
      .type = BIQUAD_TYPE_PEAKING, .freq = 0.0, .q = 1.0, .gain = 1.23};
  ASSERT_NE(0, biquad_parameters_validate(&p4, fs48, NULL));
}

TEST(ValidateSlope) {
  int fs48 = 48000;
  biquad_parameters_t p1 = {.type = BIQUAD_TYPE_HIGHSHELF,
                            .freq = 1000.0,
                            .gain = 1.23,
                            .slope = 5.0};
  ASSERT_EQ(0, biquad_parameters_validate(&p1, fs48, NULL));
  biquad_parameters_t p2 = {.type = BIQUAD_TYPE_HIGHSHELF,
                            .freq = 1000.0,
                            .gain = 1.23,
                            .slope = 0.0};
  ASSERT_NE(0, biquad_parameters_validate(&p2, fs48, NULL));
  biquad_parameters_t p3 = {.type = BIQUAD_TYPE_HIGHSHELF,
                            .freq = 1000.0,
                            .gain = 1.23,
                            .slope = 15.0};
  ASSERT_NE(0, biquad_parameters_validate(&p3, fs48, NULL));
}

TEST_MAIN()
