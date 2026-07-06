#include "convolution.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Uniform-partitioned overlap-save FIR convolution.
// Stockham-style segmented overlap-save with one 2N-point real FFT per
// chunk and an N+1-bin spectrum-domain multiply-accumulate across the
// segment history.
//
//   - Uses `RealFFT`, which stores the same N+1 unique bins as separate
//     `specRe`/`specIm` arrays. The flat layout (DC at index 0, Nyquist
//     at index N, both with `im == 0`) lets us run the spectrum
//     multiply through `vDSP_zvmulD` / `vDSP_zvmaD` without any DC/
//     Nyquist special-casing.
//   - `RealFFT.inverse` produces `length · signal`. The inverse does not
//     scale, so we pre-divide coefficients by
//     `2 * data_length` to compensate.
//   - All hot-path buffers are owned by raw `UnsafeMutablePointer`s
//     (`AudioBuffers`-style) so `process(waveform:)` cannot trip
//     Swift's Array CoW path that a `[PrcFmt]` field would.

/// Build a convolution filter from raw IR samples.
///
/// - Parameters:
///   - coefficients: Impulse response, in time-domain sample order.
///     Must be non-empty.
///   - chunkSize: Per-call block length `N`. Must match the
///     `validFrames` the pipeline will hand to `process`.
///
/// Resolve the parameters to a flat IR buffer. Only called from the
/// control plane (filter creation / hot-swap), never from
/// `process(waveform:)`.
///
/// Convenience initialiser that resolves `ConvParameters` to a flat
/// IR buffer first (control plane only, may touch the filesystem).
convolution_filter_t* convolution_filter_create(const char* name, const conv_parameters_t* params, size_t chunk_size) {
    if (!params || chunk_size == 0) return NULL;
    convolution_filter_t* filter = (convolution_filter_t*)malloc(sizeof(convolution_filter_t));
    if (!filter) return NULL;
    if (name) {
        strncpy(filter->name, name, sizeof(filter->name) - 1);
        filter->name[sizeof(filter->name) - 1] = '\0';
    } else {
        strcpy(filter->name, "convolution");
    }
    filter->chunk_size = chunk_size;
    size_t fft_len = 2 * chunk_size;
    filter->fft = real_fft_create(fft_len);
    if (!filter->fft) {
        free(filter);
        return NULL;
    }
    size_t spec_len = filter->fft->spectrum_length;

    const prc_fmt_t* coeffs = NULL;
    size_t coeffs_count = 0;
    prc_fmt_t* dummy_coeffs = NULL;

    if (params->type == CONV_TYPE_VALUES) {
        coeffs = params->values;
        coeffs_count = params->values_count;
    } else if (params->type == CONV_TYPE_DUMMY) {
        size_t len = params->length > 0 ? params->length : 1;
        dummy_coeffs = (prc_fmt_t*)calloc(len, sizeof(prc_fmt_t));
        dummy_coeffs[0] = 1.0;
        coeffs = dummy_coeffs;
        coeffs_count = len;
    }

    if (!coeffs || coeffs_count == 0) {
        if (dummy_coeffs) free(dummy_coeffs);
        real_fft_free(filter->fft);
        free(filter);
        return NULL;
    }

    size_t num_seg = (coeffs_count + chunk_size - 1) / chunk_size;
    filter->num_segments = num_seg;
    filter->spec_re = (prc_fmt_t**)malloc(num_seg * sizeof(prc_fmt_t*));
    filter->spec_im = (prc_fmt_t**)malloc(num_seg * sizeof(prc_fmt_t*));
    filter->hist_re = (prc_fmt_t**)malloc(num_seg * sizeof(prc_fmt_t*));
    filter->hist_im = (prc_fmt_t**)malloc(num_seg * sizeof(prc_fmt_t*));

    prc_fmt_t* scratch = (prc_fmt_t*)calloc(fft_len, sizeof(prc_fmt_t));
    double inv_scale = 1.0 / (double)fft_len;

    /// Pre-scale and FFT each IR segment into split-complex spectrum
    /// storage. Static so it's reusable from both `init` and
    /// `updateCoefficients`.
    for (size_t s = 0; s < num_seg; s++) {
        filter->spec_re[s] = (prc_fmt_t*)calloc(spec_len, sizeof(prc_fmt_t));
        filter->spec_im[s] = (prc_fmt_t*)calloc(spec_len, sizeof(prc_fmt_t));
        filter->hist_re[s] = (prc_fmt_t*)calloc(spec_len, sizeof(prc_fmt_t));
        filter->hist_im[s] = (prc_fmt_t*)calloc(spec_len, sizeof(prc_fmt_t));

        memset(scratch, 0, fft_len * sizeof(prc_fmt_t));
        size_t offset = s * chunk_size;
        size_t copy_len = (coeffs_count > offset) ? (coeffs_count - offset) : 0;
        if (copy_len > chunk_size) copy_len = chunk_size;
        if (copy_len > 0) {
            // Scale-and-copy into the first half; zero the rest.
            for (size_t k = 0; k < copy_len; k++) {
                scratch[k] = coeffs[offset + k] * inv_scale;
            }
        }
        real_fft_forward(filter->fft, scratch, filter->spec_re[s], filter->spec_im[s]);
    }
    free(scratch);
    if (dummy_coeffs) free(dummy_coeffs);

    filter->write_idx = 0;
    filter->overlap_buffer = (prc_fmt_t*)calloc(chunk_size, sizeof(prc_fmt_t));
    filter->time_buf = (prc_fmt_t*)calloc(fft_len, sizeof(prc_fmt_t));
    filter->spec_accum_re = (prc_fmt_t*)calloc(spec_len, sizeof(prc_fmt_t));
    filter->spec_accum_im = (prc_fmt_t*)calloc(spec_len, sizeof(prc_fmt_t));

    return filter;
}

static void process_chunk(convolution_filter_t* filter, mutable_waveform_t waveform) {
    size_t cs = filter->chunk_size;
    size_t spec_len = filter->fft->spectrum_length;
    size_t num_seg = filter->num_segments;
    size_t widx = filter->write_idx;

    // 1. Stage the new block in the first `chunkSize` samples of
    //    `inputBuf` (`time_buf`); zero the second half (the FFT zero-pad) and any
    //    short tail of the first half (when `count < chunkSize`).
    memcpy(filter->time_buf, filter->overlap_buffer, cs * sizeof(prc_fmt_t));
    memcpy(filter->time_buf + cs, waveform, cs * sizeof(prc_fmt_t));
    memcpy(filter->overlap_buffer, waveform, cs * sizeof(prc_fmt_t));

    // 2. Advance the history index and FFT the new block into that
    //    slot. The slot now holds the spectrum of `inputBuf` (`time_buf`).
    real_fft_forward(filter->fft, filter->time_buf, filter->hist_re[widx], filter->hist_im[widx]);

    memset(filter->spec_accum_re, 0, spec_len * sizeof(prc_fmt_t));
    memset(filter->spec_accum_im, 0, spec_len * sizeof(prc_fmt_t));

    // 3. Spectrum-domain multiply-accumulate across the segment
    //    history. seg=0 pairs the newest input with coeff[0]; seg=k
    //    pairs the input from `k` blocks ago with coeff[k].
    //
    //    First segment uses zvmul (writes the accumulator); subsequent
    //    segments use zvma (D = A·B + C, called in-place with C == D).
    for (size_t s = 0; s < num_seg; s++) {
        size_t hidx = (widx + num_seg - s) % num_seg;
        const prc_fmt_t* hre = filter->hist_re[hidx];
        const prc_fmt_t* him = filter->hist_im[hidx];
        const prc_fmt_t* sre = filter->spec_re[s];
        const prc_fmt_t* sim = filter->spec_im[s];
        prc_fmt_t* acc_re = filter->spec_accum_re;
        prc_fmt_t* acc_im = filter->spec_accum_im;

        for (size_t k = 0; k < spec_len; k++) {
            acc_re[k] += hre[k] * sre[k] - him[k] * sim[k];
            acc_im[k] += hre[k] * sim[k] + him[k] * sre[k];
        }
    }

    // 4. Inverse FFT. RealFFT.inverse multiplies by
    //    `length = 2N`, but `coeffsF` was pre-divided by `2N` in init,
    //    so the net result is the un-normalised linear convolution
    //    sum.
    real_fft_inverse(filter->fft, filter->spec_accum_re, filter->spec_accum_im, filter->time_buf);
    
    // 5. Overlap-save output: out[i] = ifft[i] + overlap_prev[i] for
    //    i in 0..<N; overlap_next = ifft[N..2N].
    memcpy(waveform, filter->time_buf + cs, cs * sizeof(prc_fmt_t));

    filter->write_idx = (widx + 1) % num_seg;
}

/// Process one block in-place. The hot path is allocation-free in
/// steady state; everything below is pointer arithmetic over the
/// preallocated storage from `init`.
void convolution_filter_process(convolution_filter_t* filter, mutable_waveform_t waveform, size_t count) {
    if (!filter || !waveform || count == 0) return;
    size_t cs = filter->chunk_size;
    size_t processed = 0;
    while (processed + cs <= count) {
        process_chunk(filter, waveform + processed);
        processed += cs;
    }
}

void convolution_filter_free(convolution_filter_t* filter) {
    if (!filter) return;
    if (filter->fft) {
        size_t num_seg = filter->num_segments;
        for (size_t s = 0; s < num_seg; s++) {
            if (filter->spec_re && filter->spec_re[s]) free(filter->spec_re[s]);
            if (filter->spec_im && filter->spec_im[s]) free(filter->spec_im[s]);
            if (filter->hist_re && filter->hist_re[s]) free(filter->hist_re[s]);
            if (filter->hist_im && filter->hist_im[s]) free(filter->hist_im[s]);
        }
        if (filter->spec_re) free(filter->spec_re);
        if (filter->spec_im) free(filter->spec_im);
        if (filter->hist_re) free(filter->hist_re);
        if (filter->hist_im) free(filter->hist_im);
        real_fft_free(filter->fft);
    }
    if (filter->overlap_buffer) free(filter->overlap_buffer);
    if (filter->time_buf) free(filter->time_buf);
    if (filter->spec_accum_re) free(filter->spec_accum_re);
    if (filter->spec_accum_im) free(filter->spec_accum_im);
    free(filter);
}
