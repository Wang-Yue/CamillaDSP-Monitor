/**
 * @file resampler_error.h
 * @brief Errors raised by AudioResampler implementations.
 *
 * Defines the error codes returned during construction and processing
 * by the audio resamplers.
 */

#ifndef CLIB_RESAMPLER_RESAMPLER_ERROR_H
#define CLIB_RESAMPLER_RESAMPLER_ERROR_H

/**
 * @brief Error codes returned by AudioResampler operations.
 */
typedef enum {
  /** The operation completed successfully. */
  RESAMPLER_OK = 0,
  /** The input chunk size did not equal the resampler's fixed chunk size. */
  RESAMPLER_ERR_INPUT_SIZE_MISMATCH = 1,
  /** The output buffer does not have enough capacity to hold the output frames.
   */
  RESAMPLER_ERR_OUTPUT_BUFFER_TOO_SMALL = 2,
  /** The output chunk channel count does not match the resampler channel count.
   */
  RESAMPLER_ERR_CHANNEL_COUNT_MISMATCH = 3,
  /** Invalid parameter (e.g. non-positive channel count or chunk size) passed
     to init. */
  RESAMPLER_ERR_INVALID_PARAMETER = 4,
  /** The underlying resampler failed to initialize. */
  RESAMPLER_ERR_INITIALIZATION_FAILED = 5
} resampler_error_t;

/**
 * @brief Returns a descriptive string for the given resampler error.
 *
 * @param err The error code.
 * @return A constant string describing the error.
 */
const char* resampler_error_description(resampler_error_t err);

#endif  // CLIB_RESAMPLER_RESAMPLER_ERROR_H
