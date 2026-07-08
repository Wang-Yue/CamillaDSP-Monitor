/**
 * @file spectrum_analyzer.h
 * @brief Spectrum analyzer for computing frequency response from audio history.
 *
 * This file defines the spectrum analyzer, which performs FFT on audio data
 * retrieved from an `audio_history_buffer_t` to compute the frequency spectrum
 * (frequencies and magnitudes) on demand.
 */

#ifndef CLIB_AUDIO_SPECTRUM_ANALYZER_H
#define CLIB_AUDIO_SPECTRUM_ANALYZER_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_history_buffer.h"

#ifdef ENABLE_ACCELERATE
#include <Accelerate/Accelerate.h>
#endif

/**
 * @brief Structure containing the result of a spectrum analysis.
 *
 * This structure holds pointers to arrays of bin-center frequencies and
 * their corresponding magnitudes. The lifetime of these arrays is managed
 * by the spectrum analyzer that computed them.
 */
typedef struct {
  const float* frequencies; /**< Array of bin-center frequencies in Hz. */
  const float* magnitudes;  /**< Array of magnitudes in dBFS. */
  size_t count;             /**< Number of bins in the spectrum. */
} spectrum_result_t;

/**
 * @brief Status/error codes returned by `spectrum_analyzer_compute(...)`. The
 * spectrum analyzer wraps an `audio_history_buffer_t`; channel-out-of-range
 * errors surface as `SPECTRUM_ERROR_OUT_OF_RANGE` and bubble through
 * unchanged.
 */
typedef enum {
  SPECTRUM_OK = 0, /**< Operation completed successfully. */
  SPECTRUM_ERROR_EMPTY =
      -1, /**< Not enough samples buffered to fill the FFT window. */
  SPECTRUM_ERROR_INVALID_PARAM =
      -2, /**< Invalid parameters passed to the analyzer. */
  SPECTRUM_ERROR_OUT_OF_RANGE = -3 /**< Channel index is out of range. */
} spectrum_status_t;

/**
 * @brief Opaque structure representing a spectrum analyzer.
 */
typedef struct spectrum_analyzer spectrum_analyzer_t;

/**
 * @brief Creates a new spectrum analyzer instance.
 *
 * @return Pointer to the allocated spectrum_analyzer_t structure, or NULL on
 * failure.
 */
spectrum_analyzer_t* spectrum_analyzer_create(void);

/**
 * @brief Frees the spectrum analyzer instance.
 *
 * @param analyzer Pointer to the spectrum analyzer instance to free.
 */
void spectrum_analyzer_free(spectrum_analyzer_t* analyzer);

/**
 * @brief Compute a spectrum on demand (consumer side).
 *
 * This function extracts audio data from the provided history buffer for the
 * specified channel, performs windowing and FFT, and calculates the magnitudes.
 *
 * @param analyzer Pointer to the spectrum analyzer.
 * @param buffer Pointer to the audio history buffer containing the source data.
 * @param channel The channel index to analyze.
 * @param min_freq The minimum frequency of interest.
 * @param max_freq The maximum frequency of interest.
 * @param n_bins The desired number of frequency bins in the output.
 * @param samplerate The sample rate of the audio data.
 * @param out_result Pointer to a spectrum_result_t structure to receive the
 * results.
 * @return SPECTRUM_OK on success, or an error code on failure.
 */
spectrum_status_t spectrum_analyzer_compute(spectrum_analyzer_t* analyzer,
                                            audio_history_buffer_t* buffer,
                                            int channel, double min_freq,
                                            double max_freq, size_t n_bins,
                                            size_t samplerate,
                                            spectrum_result_t* out_result);

#endif  // CLIB_AUDIO_SPECTRUM_ANALYZER_H
