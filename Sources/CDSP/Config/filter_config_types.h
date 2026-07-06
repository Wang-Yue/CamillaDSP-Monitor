#ifndef CLIB_CONFIG_FILTER_CONFIG_TYPES_H
#define CLIB_CONFIG_FILTER_CONFIG_TYPES_H

// Standalone filter configuration types.

#include "config_error.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FADER_T_DEFINED
#define FADER_T_DEFINED
typedef enum {
    FADER_MAIN = 0,
    FADER_AUX1 = 1,
    FADER_AUX2 = 2,
    FADER_AUX3 = 3,
    FADER_AUX4 = 4,
    FADER_NONE = -1
} fader_t;
#endif

const char* fader_to_string(fader_t fader);
fader_t fader_from_string(const char* str);

typedef enum {
    FILTER_TYPE_GAIN = 0,
    FILTER_TYPE_VOLUME,
    FILTER_TYPE_LOUDNESS,
    FILTER_TYPE_BIQUAD,
    FILTER_TYPE_CONV,
    FILTER_TYPE_DELAY,
    FILTER_TYPE_BIQUAD_COMBO,
    FILTER_TYPE_DIFF_EQ,
    FILTER_TYPE_DITHER,
    FILTER_TYPE_LIMITER,
    FILTER_TYPE_LOOKAHEAD_LIMITER
} filter_type_t;

const char* filter_type_to_string(filter_type_t type);
filter_type_t filter_type_from_string(const char* str);

typedef enum {
    GAIN_SCALE_DB = 0,
    GAIN_SCALE_LINEAR
} gain_scale_t;

typedef struct {
    double gain;
    bool has_gain;
    gain_scale_t scale;
    bool inverted;
    bool mute;
} gain_parameters_t;

typedef struct {
    double reference_level;
    bool has_reference_level;
    double high_boost;
    bool has_high_boost;
    double low_boost;
    bool has_low_boost;
    bool attenuate_mid;
    fader_t fader;
} loudness_parameters_t;

typedef enum {
    BIQUAD_TYPE_FREE = 0,
    BIQUAD_TYPE_HIGHPASS,
    BIQUAD_TYPE_LOWPASS,
    BIQUAD_TYPE_HIGHPASS_FO,
    BIQUAD_TYPE_LOWPASS_FO,
    BIQUAD_TYPE_HIGHSHELF,
    BIQUAD_TYPE_LOWSHELF,
    BIQUAD_TYPE_HIGHSHELF_FO,
    BIQUAD_TYPE_LOWSHELF_FO,
    BIQUAD_TYPE_PEAKING,
    BIQUAD_TYPE_NOTCH,
    BIQUAD_TYPE_BANDPASS,
    BIQUAD_TYPE_ALLPASS,
    BIQUAD_TYPE_ALLPASS_FO,
    BIQUAD_TYPE_GENERAL_NOTCH,
    BIQUAD_TYPE_LINKWITZ_TRANSFORM
} biquad_type_t;

typedef struct {
    biquad_type_t type;
    double freq;
    double gain;
    double q;
    double bandwidth;
    double slope;
    // Free biquad coefficients
    double a1, a2, b0, b1, b2;
    // GeneralNotch parameters
    double freq_notch, freq_pole, q_p;
    bool normalize_at_dc;
    // LinkwitzTransform parameters
    double freq_act, q_act, freq_target, q_target;
} biquad_parameters_t;

typedef enum {
    CONV_TYPE_VALUES = 0,
    CONV_TYPE_WAV,
    CONV_TYPE_RAW,
    CONV_TYPE_DUMMY
} conv_type_t;

typedef struct {
    conv_type_t type;
    double* values;
    size_t values_count;
    char filename[256];
    char format[32];
    int channel;
    int length;
    int skip_bytes_lines;
    int read_bytes_lines;
} conv_parameters_t;

typedef enum {
    DELAY_UNIT_MS = 0,
    DELAY_UNIT_US,
    DELAY_UNIT_SAMPLES,
    DELAY_UNIT_MM
} delay_unit_t;

typedef struct {
    double delay;
    delay_unit_t unit;
    bool subsample;
} delay_parameters_t;

typedef enum {
    BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS = 0,
    BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS,
    BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS,
    BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS,
    BIQUAD_COMBO_TYPE_TILT,
    BIQUAD_COMBO_TYPE_FIVE_POINT_PEQ,
    BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER
} biquad_combo_type_t;

typedef struct {
    biquad_combo_type_t type;
    double freq;
    bool has_freq;
    int order;
    bool has_order;
    double gain;
    bool has_gain;
    double fls, qls, gls;
    bool has_fls, has_qls, has_gls;
    double fp1, qp1, gp1;
    bool has_fp1, has_qp1, has_gp1;
    double fp2, qp2, gp2;
    bool has_fp2, has_qp2, has_gp2;
    double fp3, qp3, gp3;
    bool has_fp3, has_qp3, has_gp3;
    double fhs, qhs, ghs;
    bool has_fhs, has_qhs, has_ghs;
    double freq_min, freq_max;
    bool has_freq_min, has_freq_max;
    double* gains;
    size_t gains_count;
} biquad_combo_parameters_t;

typedef struct {
    double* a;
    size_t a_count;
    double* b;
    size_t b_count;
} diff_eq_parameters_t;

typedef enum {
    DITHER_TYPE_NONE = 0,
    DITHER_TYPE_FLAT,
    DITHER_TYPE_HIGHPASS,
    DITHER_TYPE_FWEIGHTED_441,
    DITHER_TYPE_FWEIGHTED_LONG_441,
    DITHER_TYPE_FWEIGHTED_SHORT_441,
    DITHER_TYPE_GESEMANN_441,
    DITHER_TYPE_GESEMANN_48,
    DITHER_TYPE_LIPSHITZ_441,
    DITHER_TYPE_LIPSHITZ_LONG_441,
    DITHER_TYPE_SHIBATA_441,
    DITHER_TYPE_SHIBATA_HIGH_441,
    DITHER_TYPE_SHIBATA_LOW_441,
    DITHER_TYPE_SHIBATA_48,
    DITHER_TYPE_SHIBATA_HIGH_48,
    DITHER_TYPE_SHIBATA_LOW_48,
    DITHER_TYPE_SHIBATA_882,
    DITHER_TYPE_SHIBATA_LOW_882,
    DITHER_TYPE_SHIBATA_96,
    DITHER_TYPE_SHIBATA_LOW_96,
    DITHER_TYPE_SHIBATA_192,
    DITHER_TYPE_SHIBATA_LOW_192
} dither_type_t;

typedef struct {
    dither_type_t type;
    int bits;
    double amplitude;
    bool has_amplitude;
} dither_parameters_t;

typedef struct {
    double clip_limit;
    bool soft_clip;
} limiter_parameters_t;

typedef struct {
    double limit;
    double attack;
    double release;
    delay_unit_t unit;
} lookahead_limiter_parameters_t;

typedef struct {
    double ramp_time;
    bool has_ramp_time;
    double limit;
    bool has_limit;
    fader_t fader;
} volume_parameters_t;

typedef struct {
    filter_type_t type;
    union {
        gain_parameters_t gain;
        volume_parameters_t volume;
        loudness_parameters_t loudness;
        biquad_parameters_t biquad;
        conv_parameters_t conv;
        delay_parameters_t delay;
        biquad_combo_parameters_t biquad_combo;
        diff_eq_parameters_t diff_eq;
        dither_parameters_t dither;
        limiter_parameters_t limiter;
        lookahead_limiter_parameters_t lookahead_limiter;
    } parameters;
} filter_config_t;

int biquad_parameters_validate(const biquad_parameters_t* params, int sample_rate, config_error_t* err);
int gain_parameters_validate(const gain_parameters_t* params, config_error_t* err);
int loudness_parameters_validate(const loudness_parameters_t* params, config_error_t* err);
int conv_parameters_validate(const conv_parameters_t* params, config_error_t* err);
int delay_parameters_validate(const delay_parameters_t* params, config_error_t* err);
int biquad_combo_parameters_validate(const biquad_combo_parameters_t* params, int sample_rate, config_error_t* err);
int diff_eq_parameters_validate(const diff_eq_parameters_t* params, config_error_t* err);
int dither_parameters_validate(const dither_parameters_t* params, config_error_t* err);
int limiter_parameters_validate(const limiter_parameters_t* params, config_error_t* err);
int lookahead_limiter_parameters_validate(const lookahead_limiter_parameters_t* params, int sample_rate, config_error_t* err);
int volume_parameters_validate(const volume_parameters_t* params, config_error_t* err);

int filter_config_validate(const filter_config_t* filter, int sample_rate, config_error_t* err);

#ifdef __cplusplus
}
#endif

#endif // CLIB_CONFIG_FILTER_CONFIG_TYPES_H
