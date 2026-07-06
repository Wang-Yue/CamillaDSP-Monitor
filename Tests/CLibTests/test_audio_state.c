#include <math.h>

#include "../../Sources/CDSP/Audio/audio_chunk.h"
#include "../../Sources/CDSP/Audio/double_helpers.h"
#include "../../Sources/CDSP/Audio/processing_parameters.h"
#include "test_support.h"

TEST(ProcessingParametersGettersSetters) {
  processing_parameters_t* params = processing_parameters_create(2, 2);
  ASSERT_TRUE(params != NULL);

  processing_parameters_set_target_volume(params, -10.0);
  ASSERT_DOUBLE_EQ(-10.0, processing_parameters_get_target_volume(params));

  processing_parameters_set_current_volume(params, -12.0);
  ASSERT_DOUBLE_EQ(-12.0, processing_parameters_get_current_volume(params));

  processing_parameters_set_muted(params, true);
  ASSERT_TRUE(processing_parameters_is_muted(params));

  double cap_peak[] = {-3.0, -4.0};
  processing_parameters_set_capture_signal_peak(params, cap_peak, 2);
  double out_cap_peak[2] = {0};
  processing_parameters_get_capture_signal_peak(params, out_cap_peak, 2);
  ASSERT_DOUBLE_EQ(-3.0, out_cap_peak[0]);
  ASSERT_DOUBLE_EQ(-4.0, out_cap_peak[1]);

  double cap_rms[] = {-10.0, -11.0};
  processing_parameters_set_capture_signal_rms(params, cap_rms, 2);
  double out_cap_rms[2] = {0};
  processing_parameters_get_capture_signal_rms(params, out_cap_rms, 2);
  ASSERT_DOUBLE_EQ(-10.0, out_cap_rms[0]);
  ASSERT_DOUBLE_EQ(-11.0, out_cap_rms[1]);

  double pb_peak[] = {-1.0, -2.0};
  processing_parameters_set_playback_signal_peak(params, pb_peak, 2);
  double out_pb_peak[2] = {0};
  processing_parameters_get_playback_signal_peak(params, out_pb_peak, 2);
  ASSERT_DOUBLE_EQ(-1.0, out_pb_peak[0]);
  ASSERT_DOUBLE_EQ(-2.0, out_pb_peak[1]);

  double pb_rms[] = {-8.0, -9.0};
  processing_parameters_set_playback_signal_rms(params, pb_rms, 2);
  double out_pb_rms[2] = {0};
  processing_parameters_get_playback_signal_rms(params, out_pb_rms, 2);
  ASSERT_DOUBLE_EQ(-8.0, out_pb_rms[0]);
  ASSERT_DOUBLE_EQ(-9.0, out_pb_rms[1]);

  processing_parameters_free(params);
}

TEST(ProcessingParametersMultiChannelSetters) {
  processing_parameters_t* params = processing_parameters_create(2, 2);
  ASSERT_TRUE(params != NULL);

  double cap_peak[] = {-5.0, -6.0};
  processing_parameters_set_capture_signal_peak(params, cap_peak, 2);
  double out_cap_peak[2] = {0};
  processing_parameters_get_capture_signal_peak(params, out_cap_peak, 2);
  ASSERT_DOUBLE_EQ(-5.0, out_cap_peak[0]);
  ASSERT_DOUBLE_EQ(-6.0, out_cap_peak[1]);

  double cap_rms[] = {-15.0, -16.0};
  processing_parameters_set_capture_signal_rms(params, cap_rms, 2);
  double out_cap_rms[2] = {0};
  processing_parameters_get_capture_signal_rms(params, out_cap_rms, 2);
  ASSERT_DOUBLE_EQ(-15.0, out_cap_rms[0]);
  ASSERT_DOUBLE_EQ(-16.0, out_cap_rms[1]);

  double pb_peak[] = {-2.0, -3.0};
  processing_parameters_set_playback_signal_peak(params, pb_peak, 2);
  double out_pb_peak[2] = {0};
  processing_parameters_get_playback_signal_peak(params, out_pb_peak, 2);
  ASSERT_DOUBLE_EQ(-2.0, out_pb_peak[0]);
  ASSERT_DOUBLE_EQ(-3.0, out_pb_peak[1]);

  double pb_rms[] = {-12.0, -13.0};
  processing_parameters_set_playback_signal_rms(params, pb_rms, 2);
  double out_pb_rms[2] = {0};
  processing_parameters_get_playback_signal_rms(params, out_pb_rms, 2);
  ASSERT_DOUBLE_EQ(-12.0, out_pb_rms[0]);
  ASSERT_DOUBLE_EQ(-13.0, out_pb_rms[1]);

  processing_parameters_free(params);
}

TEST(ProcessingParametersUpdateLevels) {
  processing_parameters_t* params = processing_parameters_create(2, 2);
  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  chunk->valid_frames = 1024;

  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < 1024; t++) {
      buf[t] = 1.0;
    }
  }

  double loudest_cap =
      processing_parameters_update_capture_levels(params, chunk);
  ASSERT_NEAR(0.0, loudest_cap, 1e-3);
  double out_cap_peak[2] = {0};
  processing_parameters_get_capture_signal_peak(params, out_cap_peak, 2);
  ASSERT_NEAR(0.0, out_cap_peak[0], 1e-3);
  double out_cap_rms[2] = {0};
  processing_parameters_get_capture_signal_rms(params, out_cap_rms, 2);
  ASSERT_NEAR(0.0, out_cap_rms[0], 1e-3);

  double loudest_pb =
      processing_parameters_update_playback_levels(params, chunk);
  ASSERT_NEAR(0.0, loudest_pb, 1e-3);
  double out_pb_peak[2] = {0};
  processing_parameters_get_playback_signal_peak(params, out_pb_peak, 2);
  ASSERT_NEAR(0.0, out_pb_peak[0], 1e-3);
  double out_pb_rms[2] = {0};
  processing_parameters_get_playback_signal_rms(params, out_pb_rms, 2);
  ASSERT_NEAR(0.0, out_pb_rms[0], 1e-3);

  audio_chunk_free(chunk);
  processing_parameters_free(params);
}

TEST(DSPOpsScalarMultiply) {
  double buffer[] = {1.0, 2.0, 3.0};
  dsp_ops_scalar_multiply(buffer, 2.0, 3);
  ASSERT_DOUBLE_EQ(2.0, buffer[0]);
  ASSERT_DOUBLE_EQ(4.0, buffer[1]);
  ASSERT_DOUBLE_EQ(6.0, buffer[2]);
}

TEST(DSPOpsAdd) {
  double a[] = {1.0, 2.0, 3.0};
  double b[] = {4.0, 5.0, 6.0};
  dsp_ops_add(a, b, 2);
  ASSERT_DOUBLE_EQ(5.0, b[0]);
  ASSERT_DOUBLE_EQ(7.0, b[1]);
  ASSERT_DOUBLE_EQ(6.0, b[2]);
}

TEST(DSPOpsMultiply) {
  double a[] = {1.0, 2.0, 3.0};
  double b[] = {4.0, 5.0, 6.0};
  double result[] = {0.0, 0.0, 0.0};
  for (int i = 0; i < 3; i++) result[i] = b[i];
  dsp_ops_multiply(a, result, 2);
  ASSERT_DOUBLE_EQ(4.0, result[0]);
  ASSERT_DOUBLE_EQ(10.0, result[1]);
  ASSERT_DOUBLE_EQ(6.0, result[2]);
}

TEST(DSPOpsMultiplyAdd) {
  double a[] = {1.0, 2.0, 3.0};
  double acc[] = {4.0, 5.0, 6.0};
  dsp_ops_multiply_add(a, 2.0, acc, 2);
  ASSERT_DOUBLE_EQ(6.0, acc[0]);
  ASSERT_DOUBLE_EQ(9.0, acc[1]);
  ASSERT_DOUBLE_EQ(6.0, acc[2]);
}

TEST(DSPOpsPeakAndRMS) {
  double buffer[] = {1.0, -2.0, 3.0};
  ASSERT_DOUBLE_EQ(3.0, dsp_ops_peak_absolute(buffer, 3));
  ASSERT_NEAR(sqrt(14.0 / 3.0), dsp_ops_rms(buffer, 3), 1e-5);
}

TEST_MAIN()
