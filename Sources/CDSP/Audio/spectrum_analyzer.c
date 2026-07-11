#include "Audio/spectrum_analyzer.h"

#include <math.h>

#include "Audio/float_helpers.h"
#include "FFT/real_fft.h"

typedef struct {
  int low_k;
  int high_k;
  int nearest_k;
} bin_range_t;

typedef struct {
  double min_freq;
  double max_freq;
  size_t n_bins;
  size_t samplerate;
  float* frequencies;
  bin_range_t* ranges;
  size_t capacity;
} binning_plan_t;

struct spectrum_analyzer {
  size_t fft_n;
  real_fftf_t* fft_setup;
  float* window;
  // Preallocated reusable scratch buffers to eliminate frame-by-frame
  // allocations
  float* data;
  float* realp;
  float* imagp;
  float* magnitudes;
  float* db_magnitudes;

  // Cached plan for geometric binning to eliminate transcendental operations
  binning_plan_t plan;
  float* out_magnitudes;
  size_t out_capacity;
};
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

spectrum_analyzer_t* spectrum_analyzer_create(void) {
  spectrum_analyzer_t* analyzer =
      (spectrum_analyzer_t*)calloc(1, sizeof(spectrum_analyzer_t));
  if (!analyzer) return NULL;
  analyzer->fft_n = 4096;
  analyzer->fft_setup = real_fftf_create(4096);
  analyzer->window = (float*)calloc(analyzer->fft_n, sizeof(float));
  analyzer->data = (float*)calloc(analyzer->fft_n, sizeof(float));
  analyzer->realp = (float*)calloc(analyzer->fft_n / 2 + 1, sizeof(float));
  analyzer->imagp = (float*)calloc(analyzer->fft_n / 2 + 1, sizeof(float));
  analyzer->magnitudes = (float*)calloc(analyzer->fft_n / 2 + 1, sizeof(float));
  analyzer->db_magnitudes =
      (float*)calloc(analyzer->fft_n / 2 + 1, sizeof(float));

  if (analyzer->window) {
    dsp_ops_float_hann_window(analyzer->window, analyzer->fft_n);
  }

  analyzer->out_capacity = 256;
  analyzer->plan.frequencies =
      (float*)calloc(analyzer->out_capacity, sizeof(float));
  analyzer->plan.ranges =
      (bin_range_t*)calloc(analyzer->out_capacity, sizeof(bin_range_t));
  analyzer->plan.capacity = analyzer->out_capacity;
  analyzer->out_magnitudes =
      (float*)calloc(analyzer->out_capacity, sizeof(float));

  if (!analyzer->fft_setup || !analyzer->window || !analyzer->data ||
      !analyzer->realp || !analyzer->imagp || !analyzer->magnitudes ||
      !analyzer->db_magnitudes || !analyzer->plan.frequencies ||
      !analyzer->plan.ranges || !analyzer->out_magnitudes) {
    spectrum_analyzer_free(analyzer);
    return NULL;
  }

  return analyzer;
}

void spectrum_analyzer_free(spectrum_analyzer_t* analyzer) {
  if (!analyzer) return;
  if (analyzer->fft_setup) real_fftf_free(analyzer->fft_setup);
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
  if (analyzer->out_capacity == 0 || !analyzer->plan.frequencies ||
      !analyzer->plan.ranges || !analyzer->out_magnitudes) {
    return SPECTRUM_ERROR_INVALID_PARAM;
  }
  if (n_bins == 0 || n_bins > 65536 || min_freq <= 0.0 || max_freq <= min_freq ||
      max_freq > (double)samplerate / 2.0 || samplerate == 0) {
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

  // 1. Apply Hann window in-place to reduce spectral leakage
  dsp_ops_float_multiply(analyzer->data, analyzer->window, analyzer->data,
                         analyzer->fft_n);

  // 2. Perform FFT using unified real_fftf
  size_t half_n = analyzer->fft_n / 2;
  real_fftf_forward(analyzer->fft_setup, analyzer->data, analyzer->realp,
                    analyzer->imagp);

  // 3. Compute magnitudes in dBFS directly into preallocated arrays
  float scale = 2.0f / (float)analyzer->fft_n;
  float floor_val = 1e-10f;  // Threshold to prevent log10(0)

  dsp_ops_float_zvabs(analyzer->realp, analyzer->imagp, analyzer->magnitudes,
                      half_n + 1);
  dsp_ops_float_scalar_multiply(analyzer->magnitudes, scale,
                                analyzer->magnitudes, half_n + 1);

  // Correct scaling for DC and Nyquist (they are not doubled, so scale by 1.0/N
  // instead of 2.0/N)
  analyzer->magnitudes[0] *= 0.5f;
  analyzer->magnitudes[half_n] *= 0.5f;

  // Threshold the entire magnitudes array to floor_val in-place
  dsp_ops_float_vthr(analyzer->magnitudes, floor_val, analyzer->magnitudes,
                     half_n + 1);
  // Convert the entire magnitudes array to decibels (dBFS)
  float ref = 1.0f;
  dsp_ops_float_vdbcon(analyzer->magnitudes, ref, analyzer->db_magnitudes,
                       half_n + 1);

  // 4. Geometric Binning via Cached Plan
  // Reallocate buffers if the requested number of bins exceeds current
  // capacity.
  if (n_bins > analyzer->out_capacity) {
    size_t new_cap = spsc_audio_ring_buffer_round_up_to_power_of_two(n_bins);
    if (new_cap == 0) {
      return SPECTRUM_ERROR_INVALID_PARAM;
    }
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

  // Recompute the logarithmic binning plan if parameters changed.
  // This maps output frequency bins to ranges of FFT bins.
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

      // Define frequency boundaries for this bin
      double low_log = i > 0 ? center_log - step / 2.0 : log_min;
      double high_log = i < n_bins - 1 ? center_log + step / 2.0 : log_max;

      double low_f = pow(10.0, low_log);
      double high_f = pow(10.0, high_log);

      // Convert frequency boundaries to FFT bin indices
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

  // Map FFT magnitudes to the output bins.
  // For each output bin, we take the maximum magnitude within its mapped FFT
  // bin range.
  for (size_t i = 0; i < n_bins; i++) {
    bin_range_t range = analyzer->plan.ranges[i];
    int start = range.low_k > 0 ? range.low_k : 0;
    int end =
        range.high_k < (int)(half_n + 1) ? range.high_k : (int)(half_n + 1);
    int len = end - start;

    if (len > 0) {
      analyzer->out_magnitudes[i] =
          dsp_ops_float_max(analyzer->db_magnitudes + start, len);
    } else {
      // If the range doesn't cover any FFT bin (e.g. low frequencies with small
      // FFT), fallback to the nearest FFT bin.
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
