#include "Audio/spectrum_analyzer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "FFT/real_fft.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

spectrum_analyzer_t* spectrum_analyzer_create(void) {
  spectrum_analyzer_t* analyzer =
      (spectrum_analyzer_t*)calloc(1, sizeof(spectrum_analyzer_t));
  if (!analyzer) return NULL;
  analyzer->fft_n = 4096;
#ifdef ENABLE_ACCELERATE
  analyzer->log2n = (vDSP_Length)log2(4096.0);
  analyzer->fft_setup = vDSP_create_fftsetup(analyzer->log2n, kFFTRadix2);
#else
  analyzer->fft_setup = (void*)real_fft_create(4096);
  analyzer->fft_in_d = (double*)calloc(4096, sizeof(double));
  analyzer->fft_re_d = (double*)calloc(2049, sizeof(double));
  analyzer->fft_im_d = (double*)calloc(2049, sizeof(double));
#endif
  analyzer->window = (float*)calloc(analyzer->fft_n, sizeof(float));
  analyzer->data = (float*)calloc(analyzer->fft_n, sizeof(float));
  analyzer->realp = (float*)calloc(analyzer->fft_n / 2, sizeof(float));
  analyzer->imagp = (float*)calloc(analyzer->fft_n / 2, sizeof(float));
  analyzer->magnitudes = (float*)calloc(analyzer->fft_n / 2 + 1, sizeof(float));
  analyzer->db_magnitudes =
      (float*)calloc(analyzer->fft_n / 2 + 1, sizeof(float));

#ifdef ENABLE_ACCELERATE
  if (analyzer->window) {
    vDSP_hann_window(analyzer->window, (vDSP_Length)analyzer->fft_n, 0);
  }
#else
  if (analyzer->window) {
    for (size_t i = 0; i < analyzer->fft_n; i++) {
      analyzer->window[i] =
          0.5f *
          (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)analyzer->fft_n));
    }
  }
#endif

  analyzer->out_capacity = 256;
  analyzer->plan.frequencies =
      (float*)calloc(analyzer->out_capacity, sizeof(float));
  analyzer->plan.ranges =
      (bin_range_t*)calloc(analyzer->out_capacity, sizeof(bin_range_t));
  analyzer->plan.capacity = analyzer->out_capacity;
  analyzer->out_magnitudes =
      (float*)calloc(analyzer->out_capacity, sizeof(float));

  if (!analyzer->window || !analyzer->data || !analyzer->realp ||
      !analyzer->imagp || !analyzer->magnitudes || !analyzer->db_magnitudes ||
      !analyzer->plan.frequencies || !analyzer->plan.ranges ||
      !analyzer->out_magnitudes) {
    spectrum_analyzer_free(analyzer);
    return NULL;
  }

  return analyzer;
}

void spectrum_analyzer_free(spectrum_analyzer_t* analyzer) {
  if (!analyzer) return;
#ifdef ENABLE_ACCELERATE
  if (analyzer->fft_setup) vDSP_destroy_fftsetup(analyzer->fft_setup);
#else
  if (analyzer->fft_setup) real_fft_free((real_fft_t*)analyzer->fft_setup);
  if (analyzer->fft_in_d) free(analyzer->fft_in_d);
  if (analyzer->fft_re_d) free(analyzer->fft_re_d);
  if (analyzer->fft_im_d) free(analyzer->fft_im_d);
#endif
  if (analyzer->window) free(analyzer->window);
  if (analyzer->data) free(analyzer->data);
  if (analyzer->realp) free(analyzer->realp);
  if (analyzer->imagp) free(analyzer->imagp);
  if (analyzer->magnitudes) free(analyzer->magnitudes);
  if (analyzer->db_magnitudes) free(analyzer->db_magnitudes);
  if (analyzer->plan.frequencies) free(analyzer->plan.frequencies);
  if (analyzer->plan.ranges) free(analyzer->plan.ranges);
  if (analyzer->out_magnitudes) free(analyzer->out_magnitudes);
  free(analyzer);
}

spectrum_status_t spectrum_analyzer_compute(spectrum_analyzer_t* analyzer,
                                            audio_history_buffer_t* buffer,
                                            int channel, double min_freq,
                                            double max_freq, size_t n_bins,
                                            size_t samplerate,
                                            spectrum_result_t* out_result) {
  if (!analyzer || !buffer || !out_result) return SPECTRUM_ERROR_INVALID_PARAM;
  if (n_bins == 0 || min_freq <= 0.0 || max_freq <= min_freq ||
      samplerate == 0) {
    return SPECTRUM_ERROR_INVALID_PARAM;
  }

  // Read data from history buffer directly into preallocated instance buffer
  bool enough_data = false;
  audio_history_buffer_status_t status = audio_history_buffer_read_latest(
      buffer, analyzer->data, analyzer->fft_n, channel, &enough_data);
  if (status == AUDIO_HISTORY_BUFFER_ERROR_EMPTY) {
    return SPECTRUM_ERROR_EMPTY;
  }
  if (status == AUDIO_HISTORY_BUFFER_ERROR_OUT_OF_RANGE) {
    return SPECTRUM_ERROR_OUT_OF_RANGE;
  }
  if (!enough_data) {
    return SPECTRUM_ERROR_EMPTY;
  }

  // 1. Apply Hann window in-place
#ifdef ENABLE_ACCELERATE
  vDSP_vmul(analyzer->data, 1, analyzer->window, 1, analyzer->data, 1,
            analyzer->fft_n);
#else
  for (size_t i = 0; i < analyzer->fft_n; i++) {
    analyzer->data[i] *= analyzer->window[i];
  }
#endif

  // 2. Perform FFT using reusable split-complex buffers
  size_t half_n = analyzer->fft_n / 2;
#ifdef ENABLE_ACCELERATE
  DSPSplitComplex split_complex = {analyzer->realp, analyzer->imagp};
  vDSP_ctoz((const DSPComplex*)analyzer->data, 2, &split_complex, 1,
            (vDSP_Length)half_n);
  vDSP_fft_zrip(analyzer->fft_setup, &split_complex, 1, analyzer->log2n,
                kFFTDirection_Forward);
#else
  for (size_t i = 0; i < analyzer->fft_n; i++) {
    analyzer->fft_in_d[i] = (double)analyzer->data[i];
  }
  real_fft_forward((real_fft_t*)analyzer->fft_setup, analyzer->fft_in_d,
                   analyzer->fft_re_d, analyzer->fft_im_d);
#endif

  // 3. Compute magnitudes in dB directly into preallocated arrays
  float scale = 2.0f / (float)analyzer->fft_n;
  float floor_val = 1e-10f;

#ifdef ENABLE_ACCELERATE
  // Calculate magnitudes of complex bins [1 .. half_n - 1] via vector absolute
  // value
  DSPSplitComplex split_complex1 = {analyzer->realp + 1, analyzer->imagp + 1};
  vDSP_zvabs(&split_complex1, 1, analyzer->magnitudes + 1, 1,
             (vDSP_Length)(half_n - 1));
  // Scale the complex bins
  vDSP_vsmul(analyzer->magnitudes + 1, 1, &scale, analyzer->magnitudes + 1, 1,
             (vDSP_Length)(half_n - 1));

  // Set DC and Nyquist bins
  analyzer->magnitudes[0] = fabsf(analyzer->realp[0]) / (float)analyzer->fft_n;
  analyzer->magnitudes[half_n] =
      fabsf(analyzer->imagp[0]) / (float)analyzer->fft_n;

  // Threshold the entire magnitudes array to floor_val in-place
  vDSP_vthr(analyzer->magnitudes, 1, &floor_val, analyzer->magnitudes, 1,
            (vDSP_Length)(half_n + 1));
  // Convert the entire magnitudes array to decibels (dBFS)
  float ref = 1.0f;
  vDSP_vdbcon(analyzer->magnitudes, 1, &ref, analyzer->db_magnitudes, 1,
              (vDSP_Length)(half_n + 1), 1);
#else
  analyzer->magnitudes[0] =
      fabsf((float)analyzer->fft_re_d[0]) / (float)analyzer->fft_n;
  analyzer->magnitudes[half_n] =
      fabsf((float)analyzer->fft_re_d[half_n]) / (float)analyzer->fft_n;
  for (size_t i = 1; i < half_n; i++) {
    double re = analyzer->fft_re_d[i];
    double im = analyzer->fft_im_d[i];
    analyzer->magnitudes[i] = (float)(sqrt(re * re + im * im) * (double)scale);
  }
  for (size_t i = 0; i <= half_n; i++) {
    float mag = analyzer->magnitudes[i];
    if (mag < floor_val) mag = floor_val;
    analyzer->magnitudes[i] = mag;
    analyzer->db_magnitudes[i] = 20.0f * log10f(mag);
  }
#endif

  // 4. Geometric Binning via Cached Plan
  if (n_bins > analyzer->out_capacity) {
    size_t new_cap = spsc_audio_ring_buffer_round_up_to_power_of_two(n_bins);
    float* new_freqs =
        (float*)realloc(analyzer->plan.frequencies, new_cap * sizeof(float));
    bin_range_t* new_ranges = (bin_range_t*)realloc(
        analyzer->plan.ranges, new_cap * sizeof(bin_range_t));
    float* new_mags =
        (float*)realloc(analyzer->out_magnitudes, new_cap * sizeof(float));
    if (!new_freqs || !new_ranges || !new_mags) {
      if (new_freqs) analyzer->plan.frequencies = new_freqs;
      if (new_ranges) analyzer->plan.ranges = new_ranges;
      if (new_mags) analyzer->out_magnitudes = new_mags;
      return SPECTRUM_ERROR_INVALID_PARAM;
    }
    analyzer->plan.frequencies = new_freqs;
    analyzer->plan.ranges = new_ranges;
    analyzer->out_magnitudes = new_mags;
    analyzer->plan.capacity = new_cap;
    analyzer->out_capacity = new_cap;
  }

  if (analyzer->plan.min_freq != min_freq ||
      analyzer->plan.max_freq != max_freq || analyzer->plan.n_bins != n_bins ||
      analyzer->plan.samplerate != samplerate) {
    double log_min = log10(min_freq);
    double log_max = log10(max_freq);
    double step = n_bins > 1 ? (log_max - log_min) / (double)(n_bins - 1) : 0.0;

    for (size_t i = 0; i < n_bins; i++) {
      double center_log = log_min + step * (double)i;
      double center_f = pow(10.0, center_log);
      analyzer->plan.frequencies[i] = (float)center_f;

      double low_log = i > 0 ? center_log - step / 2.0 : log_min;
      double high_log = i < n_bins - 1 ? center_log + step / 2.0 : log_max;

      double low_f = pow(10.0, low_log);
      double high_f = pow(10.0, high_log);

      int low_k =
          (int)floor(low_f * (double)analyzer->fft_n / (double)samplerate);
      int high_k =
          (int)ceil(high_f * (double)analyzer->fft_n / (double)samplerate);
      int nearest_k =
          (int)round(center_f * (double)analyzer->fft_n / (double)samplerate);

      analyzer->plan.ranges[i].low_k = low_k;
      analyzer->plan.ranges[i].high_k = high_k;
      analyzer->plan.ranges[i].nearest_k = nearest_k;
    }
    analyzer->plan.min_freq = min_freq;
    analyzer->plan.max_freq = max_freq;
    analyzer->plan.n_bins = n_bins;
    analyzer->plan.samplerate = samplerate;
  }

  for (size_t i = 0; i < n_bins; i++) {
    bin_range_t range = analyzer->plan.ranges[i];
    int start = range.low_k > 0 ? range.low_k : 0;
    int end =
        range.high_k < (int)(half_n + 1) ? range.high_k : (int)(half_n + 1);
    int len = end - start;

    if (len > 0) {
      float max_val = -200.0f;
#ifdef ENABLE_ACCELERATE
      vDSP_maxv(analyzer->db_magnitudes + start, 1, &max_val, (vDSP_Length)len);
#else
      for (int k = start; k < end; k++) {
        if (analyzer->db_magnitudes[k] > max_val)
          max_val = analyzer->db_magnitudes[k];
      }
#endif
      analyzer->out_magnitudes[i] = max_val;
    } else {
      int k = range.nearest_k;
      if (k < 0) k = 0;
      if (k > (int)half_n) k = (int)half_n;
      analyzer->out_magnitudes[i] = analyzer->db_magnitudes[k];
    }
  }

  out_result->frequencies = analyzer->plan.frequencies;
  out_result->magnitudes = analyzer->out_magnitudes;
  out_result->count = n_bins;
  return SPECTRUM_OK;
}
