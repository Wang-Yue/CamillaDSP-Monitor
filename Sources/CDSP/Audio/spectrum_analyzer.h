#ifndef CLIB_AUDIO_SPECTRUM_ANALYZER_H
#define CLIB_AUDIO_SPECTRUM_ANALYZER_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_history_buffer.h"

#ifdef ENABLE_ACCELERATE
#include <Accelerate/Accelerate.h>
#endif

/// Result of an FFT spectrum query — bin-center frequencies (Hz) and
/// magnitudes (dBFS).
typedef struct {
  const float* frequencies;
  const float* magnitudes;
  size_t count;
} spectrum_result_t;

/// Status/error codes returned by `spectrum_analyzer_compute(...)`. The
/// spectrum analyzer wraps an `audio_history_buffer_t`; channel-out-of-range
/// errors surface as `SPECTRUM_ERROR_OUT_OF_RANGE` and bubble through
/// unchanged.
typedef enum {
  SPECTRUM_OK = 0,
  /// Not enough samples buffered yet to fill an FFT window.
  SPECTRUM_ERROR_EMPTY = -1,
  /// Caller passed nonsensical FFT parameters.
  SPECTRUM_ERROR_INVALID_PARAM = -2,
  SPECTRUM_ERROR_OUT_OF_RANGE = -3
} spectrum_status_t;

typedef struct spectrum_analyzer spectrum_analyzer_t;

spectrum_analyzer_t* spectrum_analyzer_create(void);
void spectrum_analyzer_free(spectrum_analyzer_t* analyzer);

/// Compute a spectrum on demand (consumer side).
spectrum_status_t spectrum_analyzer_compute(spectrum_analyzer_t* analyzer,
                                            audio_history_buffer_t* buffer,
                                            int channel, double min_freq,
                                            double max_freq, size_t n_bins,
                                            size_t samplerate,
                                            spectrum_result_t* out_result);

#endif  // CLIB_AUDIO_SPECTRUM_ANALYZER_H
