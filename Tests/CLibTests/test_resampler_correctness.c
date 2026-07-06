#if defined(__linux__)
#define _GNU_SOURCE
#endif
#include <math.h>
#include <stdlib.h>

#include "../../Sources/CDSP/Audio/audio_chunk.h"
#include "../../Sources/CDSP/Resampler/audio_resampler.h"
#include "test_support.h"

static void make_sine(double* out, size_t n, int rate, double freq) {
  double omega = 2.0 * M_PI * freq / (double)rate;
  for (size_t i = 0; i < n; i++) {
    out[i] = sin(omega * (double)i);
  }
}

static void assert_stereo_matches_mono(resampler_type_t type,
                                       const char* interp_str) {
  int in_rate = 44100;
  int out_rate = 48000;
  size_t chunk_size = 1024;
  size_t nbr_in = 32 * chunk_size;

  double* left = (double*)calloc(nbr_in, sizeof(double));
  double* right = (double*)calloc(nbr_in, sizeof(double));
  make_sine(left, nbr_in, in_rate, 1000.0);
  make_sine(right, nbr_in, in_rate, 1500.0);

  resampler_config_t cfg_stereo;
  resampler_config_init(&cfg_stereo, type);
  if (interp_str) {
    strncpy(cfg_stereo.interpolation, interp_str,
            sizeof(cfg_stereo.interpolation) - 1);
    cfg_stereo.has_interpolation = true;
  }
  if (type == RESAMPLER_TYPE_ASYNC_SINC) {
    strncpy(cfg_stereo.profile, "Accurate", sizeof(cfg_stereo.profile) - 1);
    cfg_stereo.has_profile = true;
  }

  audio_resampler_t* stereo = audio_resampler_create_from_config(
      &cfg_stereo, in_rate, out_rate, 2, chunk_size);
  audio_resampler_t* mono_l = audio_resampler_create_from_config(
      &cfg_stereo, in_rate, out_rate, 1, chunk_size);
  audio_resampler_t* mono_r = audio_resampler_create_from_config(
      &cfg_stereo, in_rate, out_rate, 1, chunk_size);

  ASSERT_TRUE(stereo != NULL);
  ASSERT_TRUE(mono_l != NULL);
  ASSERT_TRUE(mono_r != NULL);

  size_t actual_cs = audio_resampler_get_chunk_size(stereo);
  size_t max_out_st = audio_resampler_get_max_output_frames(stereo);
  size_t max_out_m = audio_resampler_get_max_output_frames(mono_l);

  audio_chunk_t* st_in = audio_chunk_create(actual_cs, 2);
  audio_chunk_t* st_out = audio_chunk_create(max_out_st, 2);
  audio_chunk_t* ml_in = audio_chunk_create(actual_cs, 1);
  audio_chunk_t* ml_out = audio_chunk_create(max_out_m, 1);
  audio_chunk_t* mr_in = audio_chunk_create(actual_cs, 1);
  audio_chunk_t* mr_out = audio_chunk_create(max_out_m, 1);

  size_t idx = 0;
  while (idx + actual_cs <= nbr_in) {
    double* st_ch0 = audio_chunk_get_channel(st_in, 0);
    double* st_ch1 = audio_chunk_get_channel(st_in, 1);
    double* ml_ch0 = audio_chunk_get_channel(ml_in, 0);
    double* mr_ch0 = audio_chunk_get_channel(mr_in, 0);

    for (size_t i = 0; i < actual_cs; i++) {
      st_ch0[i] = left[idx + i];
      st_ch1[i] = right[idx + i];
      ml_ch0[i] = left[idx + i];
      mr_ch0[i] = right[idx + i];
    }
    st_in->valid_frames = actual_cs;
    ml_in->valid_frames = actual_cs;
    mr_in->valid_frames = actual_cs;

    resampler_error_t err_st = audio_resampler_process(stereo, st_in, st_out);
    resampler_error_t err_ml = audio_resampler_process(mono_l, ml_in, ml_out);
    resampler_error_t err_mr = audio_resampler_process(mono_r, mr_in, mr_out);

    ASSERT_EQ(RESAMPLER_OK, err_st);
    ASSERT_EQ(RESAMPLER_OK, err_ml);
    ASSERT_EQ(RESAMPLER_OK, err_mr);
    ASSERT_EQ(st_out->valid_frames, ml_out->valid_frames);
    ASSERT_EQ(st_out->valid_frames, mr_out->valid_frames);

    const double* st_o0 = audio_chunk_get_channel(st_out, 0);
    const double* st_o1 = audio_chunk_get_channel(st_out, 1);
    const double* ml_o0 = audio_chunk_get_channel(ml_out, 0);
    const double* mr_o0 = audio_chunk_get_channel(mr_out, 0);

    for (size_t i = 0; i < st_out->valid_frames; i++) {
      ASSERT_NEAR(st_o0[i], ml_o0[i], 1e-12);
      ASSERT_NEAR(st_o1[i], mr_o0[i], 1e-12);
    }

    idx += chunk_size;
  }

  audio_chunk_free(st_in);
  audio_chunk_free(st_out);
  audio_chunk_free(ml_in);
  audio_chunk_free(ml_out);
  audio_chunk_free(mr_in);
  audio_chunk_free(mr_out);
  audio_resampler_free(stereo);
  audio_resampler_free(mono_l);
  audio_resampler_free(mono_r);
  free(left);
  free(right);
}

static void assert_inout_matches(resampler_type_t type,
                                 const char* interp_str) {
  resampler_config_t cfg;
  resampler_config_init(&cfg, type);
  if (interp_str) {
    strncpy(cfg.interpolation, interp_str, sizeof(cfg.interpolation) - 1);
    cfg.has_interpolation = true;
  }
  if (type == RESAMPLER_TYPE_ASYNC_SINC) {
    strncpy(cfg.profile, "Accurate", sizeof(cfg.profile) - 1);
    cfg.has_profile = true;
  }

  size_t chunk_size = 1024;
  audio_resampler_t* res_a =
      audio_resampler_create_from_config(&cfg, 44100, 48000, 2, chunk_size);
  audio_resampler_t* res_b =
      audio_resampler_create_from_config(&cfg, 44100, 48000, 2, chunk_size);
  ASSERT_TRUE(res_a != NULL);
  ASSERT_TRUE(res_b != NULL);

  size_t actual_cs = audio_resampler_get_chunk_size(res_a);
  size_t max_out = audio_resampler_get_max_output_frames(res_a);
  audio_chunk_t* in_chunk = audio_chunk_create(actual_cs, 2);
  audio_chunk_t* out_a = audio_chunk_create(max_out, 2);
  audio_chunk_t* out_b = audio_chunk_create(max_out, 2);

  for (int c = 0; c < 8; c++) {
    double* ch0 = audio_chunk_get_channel(in_chunk, 0);
    double* ch1 = audio_chunk_get_channel(in_chunk, 1);
    for (size_t i = 0; i < actual_cs; i++) {
      ch0[i] = sin(0.1 * (double)(c * actual_cs + i));
      ch1[i] = cos(0.15 * (double)(c * actual_cs + i));
    }
    in_chunk->valid_frames = actual_cs;

    ASSERT_EQ(RESAMPLER_OK, audio_resampler_process(res_a, in_chunk, out_a));
    ASSERT_EQ(RESAMPLER_OK, audio_resampler_process(res_b, in_chunk, out_b));
    ASSERT_EQ(out_a->valid_frames, out_b->valid_frames);

    for (size_t ch = 0; ch < 2; ch++) {
      const double* o_a = audio_chunk_get_channel(out_a, ch);
      const double* o_b = audio_chunk_get_channel(out_b, ch);
      for (size_t i = 0; i < out_a->valid_frames; i++) {
        ASSERT_NEAR(o_a[i], o_b[i], 1e-12);
      }
    }
  }

  audio_chunk_free(in_chunk);
  audio_chunk_free(out_a);
  audio_chunk_free(out_b);
  audio_resampler_free(res_a);
  audio_resampler_free(res_b);
}

static void assert_rejects_too_small(resampler_type_t type,
                                     const char* interp_str) {
  resampler_config_t cfg;
  resampler_config_init(&cfg, type);
  if (interp_str) {
    strncpy(cfg.interpolation, interp_str, sizeof(cfg.interpolation) - 1);
    cfg.has_interpolation = true;
  }
  if (type == RESAMPLER_TYPE_ASYNC_SINC) {
    strncpy(cfg.profile, "Accurate", sizeof(cfg.profile) - 1);
    cfg.has_profile = true;
  }

  size_t chunk_size = 1024;
  audio_resampler_t* res =
      audio_resampler_create_from_config(&cfg, 44100, 48000, 2, chunk_size);
  ASSERT_TRUE(res != NULL);

  size_t actual_cs = audio_resampler_get_chunk_size(res);
  audio_chunk_t* in_chunk = audio_chunk_create(actual_cs, 2);
  in_chunk->valid_frames = actual_cs;
  audio_chunk_t* too_small = audio_chunk_create(64, 2);

  resampler_error_t err = audio_resampler_process(res, in_chunk, too_small);
  ASSERT_EQ(RESAMPLER_ERR_OUTPUT_BUFFER_TOO_SMALL, err);

  audio_chunk_free(in_chunk);
  audio_chunk_free(too_small);
  audio_resampler_free(res);
}

TEST(Stereo_MatchesPerChannelMono_Synchronous) {
  assert_stereo_matches_mono(RESAMPLER_TYPE_SYNCHRONOUS, NULL);
}

TEST(Stereo_MatchesPerChannelMono_AsyncPoly) {
  assert_stereo_matches_mono(RESAMPLER_TYPE_ASYNC_POLY, "Cubic");
}

TEST(Stereo_MatchesPerChannelMono_AsyncSinc) {
  assert_stereo_matches_mono(RESAMPLER_TYPE_ASYNC_SINC, NULL);
}

TEST(InoutAPI_Synchronous_MatchesAllocatingAPI) {
  assert_inout_matches(RESAMPLER_TYPE_SYNCHRONOUS, NULL);
}

TEST(InoutAPI_AsyncPoly_MatchesAllocatingAPI) {
  assert_inout_matches(RESAMPLER_TYPE_ASYNC_POLY, "Cubic");
}

TEST(InoutAPI_AsyncSinc_MatchesAllocatingAPI) {
  assert_inout_matches(RESAMPLER_TYPE_ASYNC_SINC, NULL);
}

TEST(InoutAPI_RejectsTooSmallOutputBuffer_Synchronous) {
  assert_rejects_too_small(RESAMPLER_TYPE_SYNCHRONOUS, NULL);
}

TEST(InoutAPI_RejectsTooSmallOutputBuffer_AsyncPoly) {
  assert_rejects_too_small(RESAMPLER_TYPE_ASYNC_POLY, "Cubic");
}

TEST(InoutAPI_RejectsTooSmallOutputBuffer_AsyncSinc) {
  assert_rejects_too_small(RESAMPLER_TYPE_ASYNC_SINC, NULL);
}

TEST_MAIN()
