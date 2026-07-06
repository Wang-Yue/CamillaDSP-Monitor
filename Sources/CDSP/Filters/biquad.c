#include "biquad.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


bool biquad_coefficients_compute(const biquad_parameters_t* params, int sample_rate, biquad_coefficients_t* out_coeffs) {
    if (!params || !out_coeffs || sample_rate <= 0) return false;

    double fs = (double)sample_rate;
    double freq = params->freq > 0 ? params->freq : 1000.0;
    double gain = params->gain;
    double q = params->q > 0 ? params->q : 0.707;

    double w0 = 2.0 * M_PI * freq / fs;
    double cos_w0 = cos(w0);
    double sin_w0 = sin(w0);
    double A = pow(10.0, gain / 40.0);

    // Compute effective Q if bandwidth or slope is present
    if (params->bandwidth > 0) {
        double bw = params->bandwidth;
        q = 1.0 / (2.0 * sinh(log(2.0) / 2.0 * bw * w0 / sin_w0));
    } else if (params->slope > 0) {
        double slope_s = params->slope / 12.0;
        q = 1.0 / sqrt((A + 1.0 / A) * (1.0 / slope_s - 1.0) + 2.0);
    }

    double alpha = sin_w0 / (2.0 * q);

    double b0 = 0, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

    switch (params->type) {
    case BIQUAD_TYPE_FREE:
        b0 = params->b0;
        b1 = params->b1;
        b2 = params->b2;
        a0 = 1.0;
        a1 = params->a1;
        a2 = params->a2;
        break;

    case BIQUAD_TYPE_GENERAL_NOTCH: {
        double freq_z = params->freq_notch > 0 ? params->freq_notch : 1000.0;
        double freq_p = params->freq_pole > 0 ? params->freq_pole : 1000.0;
        double q_p = params->q_p > 0 ? params->q_p : (params->q > 0 ? params->q : 0.5);
        bool normalize = params->normalize_at_dc;
        double tn_z = tan(M_PI * freq_z / fs);
        double tn_p = tan(M_PI * freq_p / fs);
        double alpha_p = tn_p / q_p;
        double tn2_p = tn_p * tn_p;
        double tn2_z = tn_z * tn_z;
        double gain_norm = normalize ? (tn2_p / tn2_z) : 1.0;
        b0 = gain_norm * (1.0 + tn2_z);
        b1 = -2.0 * gain_norm * (1.0 - tn2_z);
        b2 = gain_norm * (1.0 + tn2_z);
        a0 = 1.0 + alpha_p + tn2_p;
        a1 = -2.0 + 2.0 * tn2_p;
        a2 = 1.0 - alpha_p + tn2_p;
        break;
    }

    case BIQUAD_TYPE_LINKWITZ_TRANSFORM: {
        double freq_act = params->freq_act > 0 ? params->freq_act : 50.0;
        double q_act = params->q_act > 0 ? params->q_act : 0.707;
        double freq_target = params->freq_target > 0 ? params->freq_target : 25.0;
        double q_target = params->q_target > 0 ? params->q_target : 0.707;
        double d0i = pow(2.0 * M_PI * freq_act, 2);
        double d1i = (2.0 * M_PI * freq_act) / q_act;
        double c0i = pow(2.0 * M_PI * freq_target, 2);
        double c1i = (2.0 * M_PI * freq_target) / q_target;
        double fc = (freq_target + freq_act) / 2.0;
        double gn = 2.0 * M_PI * fc / tan(M_PI * fc / fs);
        double gn2 = gn * gn;
        double cci = c0i + gn * c1i + gn2;
        b0 = (d0i + gn * d1i + gn2) / cci;
        b1 = 2.0 * (d0i - gn2) / cci;
        b2 = (d0i - gn * d1i + gn2) / cci;
        a0 = 1.0;
        a1 = 2.0 * (c0i - gn2) / cci;
        a2 = (c0i - gn * c1i + gn2) / cci;
        break;
    }

    case BIQUAD_TYPE_PEAKING:
        b0 = 1.0 + alpha * A;
        b1 = -2.0 * cos_w0;
        b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A;
        a1 = -2.0 * cos_w0;
        a2 = 1.0 - alpha / A;
        break;

    case BIQUAD_TYPE_LOWSHELF:
        b0 = A * ((A + 1.0) - (A - 1.0) * cos_w0 + 2.0 * sqrt(A) * alpha);
        b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cos_w0);
        b2 = A * ((A + 1.0) - (A - 1.0) * cos_w0 - 2.0 * sqrt(A) * alpha);
        a0 = (A + 1.0) + (A - 1.0) * cos_w0 + 2.0 * sqrt(A) * alpha;
        a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cos_w0);
        a2 = (A + 1.0) + (A - 1.0) * cos_w0 - 2.0 * sqrt(A) * alpha;
        break;

    case BIQUAD_TYPE_HIGHSHELF:
        b0 = A * ((A + 1.0) + (A - 1.0) * cos_w0 + 2.0 * sqrt(A) * alpha);
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos_w0);
        b2 = A * ((A + 1.0) + (A - 1.0) * cos_w0 - 2.0 * sqrt(A) * alpha);
        a0 = (A + 1.0) - (A - 1.0) * cos_w0 + 2.0 * sqrt(A) * alpha;
        a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cos_w0);
        a2 = (A + 1.0) - (A - 1.0) * cos_w0 - 2.0 * sqrt(A) * alpha;
        break;

    case BIQUAD_TYPE_LOWPASS:
        b0 = (1.0 - cos_w0) / 2.0;
        b1 = 1.0 - cos_w0;
        b2 = (1.0 - cos_w0) / 2.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cos_w0;
        a2 = 1.0 - alpha;
        break;

    case BIQUAD_TYPE_HIGHPASS:
        b0 = (1.0 + cos_w0) / 2.0;
        b1 = -(1.0 + cos_w0);
        b2 = (1.0 + cos_w0) / 2.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cos_w0;
        a2 = 1.0 - alpha;
        break;

    case BIQUAD_TYPE_NOTCH:
        b0 = 1.0;
        b1 = -2.0 * cos_w0;
        b2 = 1.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cos_w0;
        a2 = 1.0 - alpha;
        break;

    case BIQUAD_TYPE_BANDPASS:
        b0 = alpha;
        b1 = 0.0;
        b2 = -alpha;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cos_w0;
        a2 = 1.0 - alpha;
        break;

    case BIQUAD_TYPE_ALLPASS:
        b0 = 1.0 - alpha;
        b1 = -2.0 * cos_w0;
        b2 = 1.0 + alpha;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cos_w0;
        a2 = 1.0 - alpha;
        break;

    case BIQUAD_TYPE_LOWPASS_FO:
        b0 = sin_w0;
        b1 = sin_w0;
        b2 = 0.0;
        a0 = sin_w0 + 1.0 + cos_w0;
        a1 = sin_w0 - 1.0 - cos_w0;
        a2 = 0.0;
        break;

    case BIQUAD_TYPE_HIGHPASS_FO:
        b0 = 1.0 + cos_w0;
        b1 = -1.0 - cos_w0;
        b2 = 0.0;
        a0 = sin_w0 + 1.0 + cos_w0;
        a1 = sin_w0 - 1.0 - cos_w0;
        a2 = 0.0;
        break;

    case BIQUAD_TYPE_LOWSHELF_FO:
        b0 = A * sin_w0 + 1.0 + cos_w0;
        b1 = A * sin_w0 - 1.0 - cos_w0;
        b2 = 0.0;
        a0 = (1.0 / A) * sin_w0 + 1.0 + cos_w0;
        a1 = (1.0 / A) * sin_w0 - 1.0 - cos_w0;
        a2 = 0.0;
        break;

    case BIQUAD_TYPE_HIGHSHELF_FO:
        b0 = sin_w0 + A + A * cos_w0;
        b1 = sin_w0 - A - A * cos_w0;
        b2 = 0.0;
        a0 = sin_w0 + (1.0 / A) + (1.0 / A) * cos_w0;
        a1 = sin_w0 - (1.0 / A) - (1.0 / A) * cos_w0;
        a2 = 0.0;
        break;

    case BIQUAD_TYPE_ALLPASS_FO:
        b0 = sin_w0 - 1.0 - cos_w0;
        b1 = sin_w0 + 1.0 + cos_w0;
        b2 = 0.0;
        a0 = sin_w0 + 1.0 + cos_w0;
        a1 = sin_w0 - 1.0 - cos_w0;
        a2 = 0.0;
        break;
    }

    if (a0 == 0.0) return false;
    out_coeffs->b0 = b0 / a0;
    out_coeffs->b1 = b1 / a0;
    out_coeffs->b2 = b2 / a0;
    out_coeffs->a1 = a1 / a0;
    out_coeffs->a2 = a2 / a0;
    return true;
}

/// Magnitude response in dB at frequency `f` (Hz). Uses the analytic
/// transfer function H(z=e^{jω}) — no time-domain simulation needed.
/// Returns 0 dB for the degenerate case where the denominator
/// vanishes.
double biquad_coefficients_gain_db(const biquad_coefficients_t* coeffs, double f, int sample_rate) {
    if (!coeffs || sample_rate <= 0) return 0.0;
    double w = 2.0 * M_PI * f / (double)sample_rate;
    double cos_w = cos(w);
    double sin_w = sin(w);
    double cos_2w = cos(2.0 * w);
    double sin_2w = sin(2.0 * w);
    double num_re = coeffs->b0 + coeffs->b1 * cos_w + coeffs->b2 * cos_2w;
    double num_im = -coeffs->b1 * sin_w - coeffs->b2 * sin_2w;
    double den_re = 1.0 + coeffs->a1 * cos_w + coeffs->a2 * cos_2w;
    double den_im = -coeffs->a1 * sin_w - coeffs->a2 * sin_2w;
    double num_mag_sq = num_re * num_re + num_im * num_im;
    double den_mag_sq = den_re * den_re + den_im * den_im;
    return (den_mag_sq > 0.0) ? 10.0 * log10(num_mag_sq / den_mag_sq) : 0.0;
}

/// Phase response in radians at frequency `f` (Hz), wrapped to
/// `(−π, π]`. Sign convention matches `atan2(Im(H), Re(H))`.
double biquad_coefficients_phase_rad(const biquad_coefficients_t* coeffs, double f, int sample_rate) {
    if (!coeffs || sample_rate <= 0) return 0.0;
    double w = 2.0 * M_PI * f / (double)sample_rate;
    double cos_w = cos(w);
    double sin_w = sin(w);
    double cos_2w = cos(2.0 * w);
    double sin_2w = sin(2.0 * w);
    double num_re = coeffs->b0 + coeffs->b1 * cos_w + coeffs->b2 * cos_2w;
    double num_im = -coeffs->b1 * sin_w - coeffs->b2 * sin_2w;
    double den_re = 1.0 + coeffs->a1 * cos_w + coeffs->a2 * cos_2w;
    double den_im = -coeffs->a1 * sin_w - coeffs->a2 * sin_2w;
    double den_mag_sq = den_re * den_re + den_im * den_im;
    if (den_mag_sq <= 0.0) return 0.0;
    double h_re = (num_re * den_re + num_im * den_im) / den_mag_sq;
    double h_im = (num_im * den_re - num_re * den_im) / den_mag_sq;
    return atan2(h_im, h_re);
}

biquad_filter_t* biquad_filter_create(const char* name, const biquad_coefficients_t* coeffs) {
    biquad_filter_t* filter = (biquad_filter_t*)malloc(sizeof(biquad_filter_t));
    if (!filter) return NULL;
    if (name) {
        strncpy(filter->name, name, sizeof(filter->name) - 1);
        filter->name[sizeof(filter->name) - 1] = '\0';
    } else {
        strcpy(filter->name, "biquad");
    }
    filter->coeffs = coeffs ? *coeffs : biquad_coefficients_passthrough();
#ifdef __APPLE__
    filter->coeffs_array[0] = filter->coeffs.b0;
    filter->coeffs_array[1] = filter->coeffs.b1;
    filter->coeffs_array[2] = filter->coeffs.b2;
    filter->coeffs_array[3] = filter->coeffs.a1;
    filter->coeffs_array[4] = filter->coeffs.a2;
    filter->setup = vDSP_biquadm_CreateSetupD(filter->coeffs_array, 1, 1);
#else
    filter->z1 = 0.0;
    filter->z2 = 0.0;
#endif
    return filter;
}

void biquad_filter_process(biquad_filter_t* filter, mutable_waveform_t waveform, size_t count) {
    if (!filter || !waveform || count == 0) return;
#ifdef __APPLE__
    if (!filter->setup) return;
    const double* signal_ptr = waveform;
    double* output_ptr = waveform;
    vDSP_biquadmD(filter->setup, &signal_ptr, 1, &output_ptr, 1, count);
#else
    double b0 = filter->coeffs.b0;
    double b1 = filter->coeffs.b1;
    double b2 = filter->coeffs.b2;
    double a1 = filter->coeffs.a1;
    double a2 = filter->coeffs.a2;
    double z1 = filter->z1;
    double z2 = filter->z2;
    for (size_t i = 0; i < count; i++) {
        double in = waveform[i];
        double out = b0 * in + z1;
        z1 = b1 * in - a1 * out + z2;
        z2 = b2 * in - a2 * out;
        waveform[i] = out;
    }
    filter->z1 = z1;
    filter->z2 = z2;
#endif
}

double biquad_filter_process_single(biquad_filter_t* filter, double sample) {
    if (!filter) return sample;
#ifdef __APPLE__
    if (!filter->setup) return sample;
    double in_val = sample;
    double out_val = 0.0;
    const double* signal_ptr = &in_val;
    double* dest_ptr = &out_val;
    vDSP_biquadmD(filter->setup, &signal_ptr, 1, &dest_ptr, 1, 1);
    return out_val;
#else
    double b0 = filter->coeffs.b0;
    double b1 = filter->coeffs.b1;
    double b2 = filter->coeffs.b2;
    double a1 = filter->coeffs.a1;
    double a2 = filter->coeffs.a2;
    double out = b0 * sample + filter->z1;
    filter->z1 = b1 * sample - a1 * out + filter->z2;
    filter->z2 = b2 * sample - a2 * out;
    return out;
#endif
}

void biquad_filter_update_parameters(biquad_filter_t* filter, const filter_config_t* config, int sample_rate) {
    if (!filter || !config) return;
    if (config->type != FILTER_TYPE_BIQUAD) return;
    biquad_coefficients_t new_coeffs;
    if (biquad_coefficients_compute(&config->parameters.biquad, sample_rate, &new_coeffs)) {
        filter->coeffs = new_coeffs;
#ifdef __APPLE__
        if (filter->setup) {
            filter->coeffs_array[0] = new_coeffs.b0;
            filter->coeffs_array[1] = new_coeffs.b1;
            filter->coeffs_array[2] = new_coeffs.b2;
            filter->coeffs_array[3] = new_coeffs.a1;
            filter->coeffs_array[4] = new_coeffs.a2;
            vDSP_biquadm_SetCoefficientsDoubleD(filter->setup, filter->coeffs_array, 0, 0, 1, 1);
        }
#endif
    }
}

void biquad_filter_free(biquad_filter_t* filter) {
    if (!filter) return;
#ifdef __APPLE__
    if (filter->setup) {
        vDSP_biquadm_DestroySetupD(filter->setup);
    }
#endif
    free(filter);
}
