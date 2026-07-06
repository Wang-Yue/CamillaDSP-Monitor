// Errors raised by AudioResampler implementations during construction
// and the per-chunk process(...) call.

#ifndef CLIB_RESAMPLER_RESAMPLER_ERROR_H
#define CLIB_RESAMPLER_RESAMPLER_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

/// Errors raised by `AudioResampler` implementations during construction
/// and the per-chunk `process(...)` call.
typedef enum {
    RESAMPLER_OK = 0,
    /// `input.validFrames` did not equal the resampler's fixed `chunkSize`.
    RESAMPLER_ERR_INPUT_SIZE_MISMATCH = 1,
    /// Caller's output AudioChunk doesn't have enough capacity per channel.
    RESAMPLER_ERR_OUTPUT_BUFFER_TOO_SMALL = 2,
    /// Caller's output AudioChunk has the wrong channel count.
    RESAMPLER_ERR_CHANNEL_COUNT_MISMATCH = 3,
    /// Caller passed a non-positive `channels` or `chunkSize` to init.
    RESAMPLER_ERR_INVALID_PARAMETER = 4,
    /// The underlying system resampler refused to initialise — typically
    /// `AudioConverterNew` returning a non-zero `OSStatus`.
    RESAMPLER_ERR_INITIALIZATION_FAILED = 5
} resampler_error_t;

/// Returns a description string for the given resampler error.
const char* resampler_error_description(resampler_error_t err);

#ifdef __cplusplus
}
#endif

#endif // CLIB_RESAMPLER_RESAMPLER_ERROR_H
