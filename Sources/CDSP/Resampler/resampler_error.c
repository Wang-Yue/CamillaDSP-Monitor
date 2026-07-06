// Errors raised by AudioResampler implementations during construction
// and the per-chunk process(...) call.

#include "resampler_error.h"

/// Returns a description string for the given resampler error.
const char* resampler_error_description(resampler_error_t err) {
  switch (err) {
    case RESAMPLER_OK:
      return "No error";
    case RESAMPLER_ERR_INPUT_SIZE_MISMATCH:
      return "Resampler input size mismatch";
    case RESAMPLER_ERR_OUTPUT_BUFFER_TOO_SMALL:
      return "Resampler output buffer too small";
    case RESAMPLER_ERR_CHANNEL_COUNT_MISMATCH:
      return "Resampler channel count mismatch";
    case RESAMPLER_ERR_INVALID_PARAMETER:
      return "Resampler invalid parameter";
    case RESAMPLER_ERR_INITIALIZATION_FAILED:
      return "Resampler initialization failed";
    default:
      return "Unknown resampler error";
  }
}
