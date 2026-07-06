// Internal processing precision type
// Default is Double (f64). Change to Float for 32-bit processing.

#ifndef CLIB_AUDIO_DOUBLE_HELPERS_H
#define CLIB_AUDIO_DOUBLE_HELPERS_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// A high-performance descriptive view of a single channel's mutable buffer pointer
typedef double* mutable_waveform_t;
/// A high-performance descriptive view of a single channel's buffer pointer
typedef const double* waveform_t;

/// Convert dB to linear gain
static inline double double_from_db(double db) {
    return pow(10.0, db / 20.0);
}

/// Convert linear gain to dB. Returns -1000.0 for zero/negative input.
static inline double doubleo_db(double linear) {
    if (linear <= 0.0) return -1000.0;
    return 20.0 * log10(linear);
}

/// Vectorized DSP operations using Apple Accelerate (vDSP) or fallback C loops.
///
/// The partial-count ops (`add`, `multiply`, `multiply_add`) need to
/// operate on the first `count` elements of buffers that may be longer
/// (chunks have a `valid_frames` <= `frames`).
///
/// In C, these accept pointers directly so callers holding stable pointers
/// (e.g. an `audio_buffers_t` channel view) avoid any copy overhead or
/// ownership checks on the audio thread.

/// Multiply vector by scalar in-place: buffer[i] *= scalar for i < count.
static inline void dsp_ops_scalar_multiply(mutable_waveform_t buffer, double scalar, size_t count) {
#ifdef __APPLE__
    vDSP_vsmulD(buffer, 1, &scalar, buffer, 1, count);
#else
    for (size_t i = 0; i < count; i++) {
        buffer[i] *= scalar;
    }
#endif
}

/// Zero `count` samples in-place.
static inline void dsp_ops_clear(mutable_waveform_t buffer, size_t count) {
#ifdef __APPLE__
    vDSP_vclrD(buffer, 1, count);
#else
    for (size_t i = 0; i < count; i++) {
        buffer[i] = 0.0;
    }
#endif
}

/// Add `a[0..<count]` into `b[0..<count]` (in-place on `b`). Must satisfy `count <= a.count` and `count <= b.count`.
static inline void dsp_ops_add(waveform_t a, mutable_waveform_t b, size_t count) {
#ifdef __APPLE__
    vDSP_vaddD(a, 1, b, 1, b, 1, count);
#else
    for (size_t i = 0; i < count; i++) {
        b[i] += a[i];
    }
#endif
}

/// Multiply two vectors element-wise: b[0..<count] = a[0..<count] * b[0..<count]
static inline void dsp_ops_multiply(waveform_t a, mutable_waveform_t b, size_t count) {
#ifdef __APPLE__
    vDSP_vmulD(a, 1, b, 1, b, 1, count);
#else
    for (size_t i = 0; i < count; i++) {
        b[i] *= a[i];
    }
#endif
}

/// Multiply-accumulate: accumulator[0..<count] += a[0..<count] * scalar
static inline void dsp_ops_multiply_add(waveform_t a, double scalar, mutable_waveform_t accumulator, size_t count) {
#ifdef __APPLE__
    // result = (a * scalar) + accumulator, written into accumulator.
    vDSP_vsmaD(a, 1, &scalar, accumulator, 1, accumulator, 1, count);
#else
    for (size_t i = 0; i < count; i++) {
        // result = (a * scalar) + accumulator, written into accumulator.
        accumulator[i] += a[i] * scalar;
    }
#endif
}

/// Find peak absolute value across the first `count` samples of the buffer.
static inline double dsp_ops_peak_absolute(waveform_t buffer, size_t count) {
    if (count == 0) return 0.0;
#ifdef __APPLE__
    double res = 0.0;
    vDSP_maxmgvD(buffer, 1, &res, count);
    return res;
#else
    double res = 0.0;
    for (size_t i = 0; i < count; i++) {
        double val = fabs(buffer[i]);
        if (val > res) res = val;
    }
    return res;
#endif
}

/// Compute root-mean-square over the first `count` samples of the buffer.
static inline double dsp_ops_rms(waveform_t buffer, size_t count) {
    if (count == 0) return 0.0;
#ifdef __APPLE__
    double res = 0.0;
    vDSP_rmsqvD(buffer, 1, &res, count);
    return res;
#else
    double sum = 0.0;
    for (size_t i = 0; i < count; i++) {
        sum += buffer[i] * buffer[i];
    }
    return sqrt(sum / count);
#endif
}

#ifdef __cplusplus
}
#endif

#endif // CLIB_AUDIO_DOUBLE_HELPERS_H
