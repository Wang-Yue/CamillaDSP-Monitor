/**
 * @file filter_config_types.h
 * @brief Configuration structures and validation functions for various audio
 * filters.
 */

#ifndef CLIB_CONFIG_FILTER_CONFIG_TYPES_H
#define CLIB_CONFIG_FILTER_CONFIG_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config_error.h"

#ifndef FADER_T_DEFINED
#define FADER_T_DEFINED
/**
 * @brief Fader channels.
 */
typedef enum {
  FADER_MAIN = 0, /**< Main fader. */
  FADER_AUX1 = 1, /**< Auxiliary fader 1. */
  FADER_AUX2 = 2, /**< Auxiliary fader 2. */
  FADER_AUX3 = 3, /**< Auxiliary fader 3. */
  FADER_AUX4 = 4, /**< Auxiliary fader 4. */
  FADER_NONE = -1 /**< No fader. */
} fader_t;
#endif

/**
 * @brief Converts a fader enum value to its string representation.
 * @param fader The fader enum value.
 * @return String representation of the fader.
 */
const char* fader_to_string(fader_t fader);

/**
 * @brief Converts a string representation of a fader to its enum value.
 * @param str The string representation.
 * @return The fader enum value.
 */
fader_t fader_from_string(const char* str);

/**
 * @brief Supported filter types.
 */
typedef enum {
  FILTER_TYPE_GAIN = 0,     /**< Gain filter. */
  FILTER_TYPE_VOLUME,       /**< Volume control filter. */
  FILTER_TYPE_LOUDNESS,     /**< Loudness compensation filter. */
  FILTER_TYPE_BIQUAD,       /**< Biquad filter (parametric EQ, shelf, etc.). */
  FILTER_TYPE_CONV,         /**< Convolution (FIR) filter. */
  FILTER_TYPE_DELAY,        /**< Delay filter. */
  FILTER_TYPE_BIQUAD_COMBO, /**< Combination of biquad filters (e.g.,
                               crossovers). */
  FILTER_TYPE_DIFF_EQ,      /**< Difference equation filter. */
  FILTER_TYPE_DITHER,       /**< Dither filter. */
  FILTER_TYPE_LIMITER,      /**< Limiter filter. */
  FILTER_TYPE_LOOKAHEAD_LIMITER /**< Lookahead limiter filter. */
} filter_type_t;

/**
 * @brief Converts a filter type enum value to its string representation.
 * @param type The filter type.
 * @return String representation of the filter type.
 */
const char* filter_type_to_string(filter_type_t type);

/**
 * @brief Converts a string representation of a filter type to its enum value.
 * @param str The string representation.
 * @return The filter type enum value.
 */
filter_type_t filter_type_from_string(const char* str);

/**
 * @brief Scale used for gain values.
 */
typedef enum {
  GAIN_SCALE_DB = 0, /**< Decibels (dB). */
  GAIN_SCALE_LINEAR  /**< Linear scale. */
} gain_scale_t;

/**
 * @brief Parameters for a Gain filter.
 */
typedef struct {
  double gain;        /**< Gain value. */
  bool has_gain;      /**< True if `gain` is specified. */
  gain_scale_t scale; /**< Scale of the gain value (dB or linear). */
  bool inverted;      /**< True if phase is inverted. */
  bool mute;          /**< True if muted. */
} gain_parameters_t;

/**
 * @brief Parameters for a Loudness compensation filter.
 */
typedef struct {
  double reference_level;   /**< Reference playback level. */
  bool has_reference_level; /**< True if `reference_level` is specified. */
  double high_boost;        /**< Maximum boost for high frequencies. */
  bool has_high_boost;      /**< True if `high_boost` is specified. */
  double low_boost;         /**< Maximum boost for low frequencies. */
  bool has_low_boost;       /**< True if `low_boost` is specified. */
  bool attenuate_mid;       /**< True to attenuate mid frequencies. */
  fader_t fader;            /**< Fader associated with this loudness filter. */
} loudness_parameters_t;

/**
 * @brief Biquad filter types.
 */
typedef enum {
  BIQUAD_TYPE_FREE = 0,          /**< Free biquad with custom coefficients. */
  BIQUAD_TYPE_HIGHPASS,          /**< High-pass filter. */
  BIQUAD_TYPE_LOWPASS,           /**< Low-pass filter. */
  BIQUAD_TYPE_HIGHPASS_FO,       /**< First-order high-pass filter. */
  BIQUAD_TYPE_LOWPASS_FO,        /**< First-order low-pass filter. */
  BIQUAD_TYPE_HIGHSHELF,         /**< High-shelf filter. */
  BIQUAD_TYPE_LOWSHELF,          /**< Low-shelf filter. */
  BIQUAD_TYPE_HIGHSHELF_FO,      /**< First-order high-shelf filter. */
  BIQUAD_TYPE_LOWSHELF_FO,       /**< First-order low-shelf filter. */
  BIQUAD_TYPE_PEAKING,           /**< Peaking EQ filter. */
  BIQUAD_TYPE_NOTCH,             /**< Notch filter. */
  BIQUAD_TYPE_BANDPASS,          /**< Band-pass filter. */
  BIQUAD_TYPE_ALLPASS,           /**< All-pass filter. */
  BIQUAD_TYPE_ALLPASS_FO,        /**< First-order all-pass filter. */
  BIQUAD_TYPE_GENERAL_NOTCH,     /**< General notch filter. */
  BIQUAD_TYPE_LINKWITZ_TRANSFORM /**< Linkwitz Transform filter. */
} biquad_type_t;

/**
 * @brief How biquad steepness is specified.
 */
typedef enum {
  STEEPNESS_TYPE_Q = 0,     /**< Quality factor (Q). */
  STEEPNESS_TYPE_BANDWIDTH, /**< Bandwidth in octaves. */
  STEEPNESS_TYPE_SLOPE      /**< Slope (shelf filters). */
} steepness_type_t;

/**
 * @brief Parameters for a Biquad filter.
 */
typedef struct {
  biquad_type_t type; /**< Type of biquad filter. */
  double freq;        /**< Center/cutoff frequency. */
  double gain;        /**< Gain (for peaking/shelf filters). */
  double q;           /**< Q factor. */
  double bandwidth;   /**< Bandwidth. */
  double slope;       /**< Slope. */
  // Free biquad coefficients
  double a1; /**< Coefficient a1 (Free biquad). */
  double a2; /**< Coefficient a2 (Free biquad). */
  double b0; /**< Coefficient b0 (Free biquad). */
  double b1; /**< Coefficient b1 (Free biquad). */
  double b2; /**< Coefficient b2 (Free biquad). */
  // GeneralNotch parameters
  double freq_notch;    /**< Notch frequency (GeneralNotch). */
  double freq_pole;     /**< Pole frequency (GeneralNotch). */
  double q_p;           /**< Pole Q factor (GeneralNotch). */
  bool normalize_at_dc; /**< Normalize gain at DC (GeneralNotch). */
  // LinkwitzTransform parameters
  double freq_act;    /**< Actual resonance frequency (LinkwitzTransform). */
  double q_act;       /**< Actual Q factor (LinkwitzTransform). */
  double freq_target; /**< Target resonance frequency (LinkwitzTransform). */
  double q_target;    /**< Target Q factor (LinkwitzTransform). */
  steepness_type_t steepness_type; /**< How steepness is specified. */
} biquad_parameters_t;

/**
 * @brief Convolution source type.
 */
typedef enum {
  CONV_TYPE_VALUES = 0, /**< Directly specified coefficient values. */
  CONV_TYPE_WAV,        /**< WAV file. */
  CONV_TYPE_RAW,        /**< Raw binary file. */
  CONV_TYPE_DUMMY       /**< Dummy filter (no-op). */
} conv_type_t;

/**
 * @brief Parameters for a Convolution filter.
 */
typedef struct {
  conv_type_t type; /**< Source type. */
  double* values;   /**< Array of coefficient values (if CONV_TYPE_VALUES). */
  size_t values_count; /**< Number of values. */
  char filename[256];  /**< Filename of the impulse response (if file-based). */
  char format[32];     /**< Sample format (for raw files). */
  int channel;         /**< Channel to use from multi-channel files. */
  int length;          /**< Max number of samples to read. */
  int skip_bytes_lines; /**< Bytes/lines to skip at start of file. */
  int read_bytes_lines; /**< Bytes/lines to read from file. */
} conv_parameters_t;

/**
 * @brief Units for delay values.
 */
typedef enum {
  DELAY_UNIT_MS = 0,  /**< Milliseconds. */
  DELAY_UNIT_US,      /**< Microseconds. */
  DELAY_UNIT_SAMPLES, /**< Samples. */
  DELAY_UNIT_MM       /**< Millimeters (distance). */
} delay_unit_t;

/**
 * @brief Computes delay in samples from a given delay value and unit.
 *
 * @param delay The delay value.
 * @param unit The unit of the delay (ms, us, samples, or mm).
 * @param sample_rate The current audio sample rate.
 * @return The equivalent delay in fractional samples.
 */
double compute_delay_samples(double delay, delay_unit_t unit, int sample_rate);

/**
 * @brief Parameters for a Delay filter.
 */
typedef struct {
  double delay;      /**< Delay value. */
  delay_unit_t unit; /**< Unit of the delay value. */
  bool subsample;    /**< Enable fractional delay (subsample). */
} delay_parameters_t;

/**
 * @brief Biquad combination types (complex filters made of multiple biquads).
 */
typedef enum {
  BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS =
      0,                                 /**< Butterworth high-pass filter. */
  BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS, /**< Butterworth low-pass filter. */
  BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS, /**< Linkwitz-Riley high-pass
                                                filter. */
  BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS, /**< Linkwitz-Riley low-pass filter.
                                             */
  BIQUAD_COMBO_TYPE_TILT,                   /**< Tilt equalizer. */
  BIQUAD_COMBO_TYPE_FIVE_POINT_PEQ,         /**< 5-band parametric EQ. */
  BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER       /**< Graphic equalizer. */
} biquad_combo_type_t;

/**
 * @brief Parameters for a Biquad Combo filter.
 */
typedef struct {
  biquad_combo_type_t type; /**< Type of combination. */
  double freq;              /**< Cutoff frequency. */
  bool has_freq;            /**< True if `freq` is specified. */
  int order;                /**< Filter order. */
  bool has_order;           /**< True if `order` is specified. */
  double gain;              /**< Overall gain or tilt gain. */
  bool has_gain;            /**< True if `gain` is specified. */
  // PEQ parameters
  double fls, qls, gls; /**< Low shelf: freq, Q, gain. */
  bool has_fls, has_qls, has_gls;
  double fp1, qp1, gp1; /**< Peak 1: freq, Q, gain. */
  bool has_fp1, has_qp1, has_gp1;
  double fp2, qp2, gp2; /**< Peak 2: freq, Q, gain. */
  bool has_fp2, has_qp2, has_gp2;
  double fp3, qp3, gp3; /**< Peak 3: freq, Q, gain. */
  bool has_fp3, has_qp3, has_gp3;
  double fhs, qhs, ghs; /**< High shelf: freq, Q, gain. */
  bool has_fhs, has_qhs, has_ghs;
  // Graphic EQ parameters
  double freq_min; /**< Minimum frequency. */
  double freq_max; /**< Maximum frequency. */
  bool has_freq_min, has_freq_max;
  double* gains;      /**< Band gains. */
  size_t gains_count; /**< Number of bands. */
} biquad_combo_parameters_t;

/**
 * @brief Parameters for a Difference Equation filter.
 */
typedef struct {
  double* a;      /**< Feedback coefficients (a). */
  size_t a_count; /**< Number of feedback coefficients. */
  double* b;      /**< Feedforward coefficients (b). */
  size_t b_count; /**< Number of feedforward coefficients. */
} diff_eq_parameters_t;

/**
 * @brief Dither noise shaping types.
 */
typedef enum {
  DITHER_TYPE_NONE = 0,            /**< No noise shaping. */
  DITHER_TYPE_FLAT,                /**< Flat dither. */
  DITHER_TYPE_HIGHPASS,            /**< High-pass dither. */
  DITHER_TYPE_FWEIGHTED_441,       /**< F-weighted for 44.1 kHz. */
  DITHER_TYPE_FWEIGHTED_LONG_441,  /**< F-weighted long for 44.1 kHz. */
  DITHER_TYPE_FWEIGHTED_SHORT_441, /**< F-weighted short for 44.1 kHz. */
  DITHER_TYPE_GESEMANN_441,        /**< Gesemann for 44.1 kHz. */
  DITHER_TYPE_GESEMANN_48,         /**< Gesemann for 48 kHz. */
  DITHER_TYPE_LIPSHITZ_441,        /**< Lipshitz for 44.1 kHz. */
  DITHER_TYPE_LIPSHITZ_LONG_441,   /**< Lipshitz long for 44.1 kHz. */
  DITHER_TYPE_SHIBATA_441,         /**< Shibata for 44.1 kHz. */
  DITHER_TYPE_SHIBATA_HIGH_441,    /**< Shibata high for 44.1 kHz. */
  DITHER_TYPE_SHIBATA_LOW_441,     /**< Shibata low for 44.1 kHz. */
  DITHER_TYPE_SHIBATA_48,          /**< Shibata for 48 kHz. */
  DITHER_TYPE_SHIBATA_HIGH_48,     /**< Shibata high for 48 kHz. */
  DITHER_TYPE_SHIBATA_LOW_48,      /**< Shibata low for 48 kHz. */
  DITHER_TYPE_SHIBATA_882,         /**< Shibata for 88.2 kHz. */
  DITHER_TYPE_SHIBATA_LOW_882,     /**< Shibata low for 88.2 kHz. */
  DITHER_TYPE_SHIBATA_96,          /**< Shibata for 96 kHz. */
  DITHER_TYPE_SHIBATA_LOW_96,      /**< Shibata low for 96 kHz. */
  DITHER_TYPE_SHIBATA_192,         /**< Shibata for 192 kHz. */
  DITHER_TYPE_SHIBATA_LOW_192      /**< Shibata low for 192 kHz. */
} dither_type_t;

/**
 * @brief Parameters for a Dither filter.
 */
typedef struct {
  dither_type_t type; /**< Dither type. */
  int bits;           /**< Target bit depth. */
  double amplitude;   /**< Dither amplitude. */
  bool has_amplitude; /**< True if `amplitude` is specified. */
} dither_parameters_t;

/**
 * @brief Parameters for a Limiter filter.
 */
typedef struct {
  double clip_limit; /**< Clip limit (linear scale). */
  bool soft_clip;    /**< Enable soft clipping. */
} limiter_parameters_t;

/**
 * @brief Parameters for a Lookahead Limiter filter.
 */
typedef struct {
  double limit;      /**< Limit value. */
  double attack;     /**< Attack time. */
  double release;    /**< Release time. */
  delay_unit_t unit; /**< Unit for attack and release times. */
} lookahead_limiter_parameters_t;

/**
 * @brief Parameters for a Volume control filter.
 */
typedef struct {
  double ramp_time;   /**< Volume ramp time (milliseconds). */
  bool has_ramp_time; /**< True if `ramp_time` is specified. */
  double limit;       /**< Volume limit (dB). */
  bool has_limit;     /**< True if `limit` is specified. */
  fader_t fader;      /**< Fader associated with this volume control. */
} volume_parameters_t;

/**
 * @brief Top-level filter configuration union.
 */
typedef struct {
  filter_type_t type; /**< Type of the filter. */
  union {
    gain_parameters_t gain;                 /**< Gain parameters. */
    volume_parameters_t volume;             /**< Volume parameters. */
    loudness_parameters_t loudness;         /**< Loudness parameters. */
    biquad_parameters_t biquad;             /**< Biquad parameters. */
    conv_parameters_t conv;                 /**< Convolution parameters. */
    delay_parameters_t delay;               /**< Delay parameters. */
    biquad_combo_parameters_t biquad_combo; /**< Biquad combo parameters. */
    diff_eq_parameters_t diff_eq; /**< Difference equation parameters. */
    dither_parameters_t dither;   /**< Dither parameters. */
    limiter_parameters_t limiter; /**< Limiter parameters. */
    lookahead_limiter_parameters_t
        lookahead_limiter; /**< Lookahead limiter parameters. */
  } parameters;            /**< Filter-specific parameters. */
} filter_config_t;

/**
 * @brief Validates biquad parameters.
 * @param params The parameters to validate.
 * @param sample_rate The current sample rate.
 * @param err Location to write error info.
 * @return 0 if valid, non-zero if invalid.
 */
int biquad_parameters_validate(const biquad_parameters_t* params,
                               int sample_rate, config_error_t* err);

/**
 * @brief Validates gain parameters.
 * @param params The parameters to validate.
 * @param err Location to write error info.
 * @return 0 if valid, non-zero if invalid.
 */
int gain_parameters_validate(const gain_parameters_t* params,
                             config_error_t* err);

/**
 * @brief Validates loudness parameters.
 * @param params The parameters to validate.
 * @param err Location to write error info.
 * @return 0 if valid, non-zero if invalid.
 */
int loudness_parameters_validate(const loudness_parameters_t* params,
                                 config_error_t* err);

/**
 * @brief Validates convolution parameters.
 * @param params The parameters to validate.
 * @param err Location to write error info.
 * @return 0 if valid, non-zero if invalid.
 */
int conv_parameters_validate(const conv_parameters_t* params,
                             config_error_t* err);

/**
 * @brief Validates delay parameters.
 * @param params The parameters to validate.
 * @param err Location to write error info.
 * @return 0 if valid, non-zero if invalid.
 */
int delay_parameters_validate(const delay_parameters_t* params,
                              config_error_t* err);

/**
 * @brief Validates biquad combo parameters.
 * @param params The parameters to validate.
 * @param sample_rate The current sample rate.
 * @param err Location to write error info.
 * @return 0 if valid, non-zero if invalid.
 */
int biquad_combo_parameters_validate(const biquad_combo_parameters_t* params,
                                     int sample_rate, config_error_t* err);

/**
 * @brief Validates difference equation parameters.
 * @param params The parameters to validate.
 * @param err Location to write error info.
 * @return 0 if valid, non-zero if invalid.
 */
int diff_eq_parameters_validate(const diff_eq_parameters_t* params,
                                config_error_t* err);

/**
 * @brief Validates dither parameters.
 * @param params The parameters to validate.
 * @param err Location to write error info.
 * @return 0 if valid, non-zero if invalid.
 */
int dither_parameters_validate(const dither_parameters_t* params,
                               config_error_t* err);

/**
 * @brief Validates limiter parameters.
 * @param params The parameters to validate.
 * @param err Location to write error info.
 * @return 0 if valid, non-zero if invalid.
 */
int limiter_parameters_validate(const limiter_parameters_t* params,
                                config_error_t* err);

/**
 * @brief Validates lookahead limiter parameters.
 * @param params The parameters to validate.
 * @param sample_rate The current sample rate.
 * @param err Location to write error info.
 * @return 0 if valid, non-zero if invalid.
 */
int lookahead_limiter_parameters_validate(
    const lookahead_limiter_parameters_t* params, int sample_rate,
    config_error_t* err);

/**
 * @brief Validates volume parameters.
 * @param params The parameters to validate.
 * @param err Location to write error info.
 * @return 0 if valid, non-zero if invalid.
 */
int volume_parameters_validate(const volume_parameters_t* params,
                               config_error_t* err);

/**
 * @brief Validates a filter configuration.
 * @param filter The filter configuration to validate.
 * @param sample_rate The current sample rate.
 * @param err Location to write error info.
 * @return 0 if valid, non-zero if invalid.
 */
int filter_config_validate(const filter_config_t* filter, int sample_rate,
                           config_error_t* err);

#endif  // CLIB_CONFIG_FILTER_CONFIG_TYPES_H
