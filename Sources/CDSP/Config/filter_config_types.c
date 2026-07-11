#include "filter_config_types.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "Filters/biquad.h"

// Standalone filter configuration types.

const char* fader_to_string(fader_t fader) {
  switch (fader) {
    case FADER_MAIN:
      return "Main";
    case FADER_AUX1:
      return "Aux1";
    case FADER_AUX2:
      return "Aux2";
    case FADER_AUX3:
      return "Aux3";
    case FADER_AUX4:
      return "Aux4";
    default:
      return "Main";
  }
}

fader_t fader_from_string(const char* str) {
  if (!str) return FADER_NONE;
  if (strcasecmp(str, "main") == 0) return FADER_MAIN;
  if (strcasecmp(str, "aux1") == 0) return FADER_AUX1;
  if (strcasecmp(str, "aux2") == 0) return FADER_AUX2;
  if (strcasecmp(str, "aux3") == 0) return FADER_AUX3;
  if (strcasecmp(str, "aux4") == 0) return FADER_AUX4;
  return FADER_NONE;
}

const char* filter_type_to_string(filter_type_t type) {
  switch (type) {
    case FILTER_TYPE_GAIN:
      return "Gain";
    case FILTER_TYPE_VOLUME:
      return "Volume";
    case FILTER_TYPE_LOUDNESS:
      return "Loudness";
    case FILTER_TYPE_BIQUAD:
      return "Biquad";
    case FILTER_TYPE_CONV:
      return "Conv";
    case FILTER_TYPE_DELAY:
      return "Delay";
    case FILTER_TYPE_BIQUAD_COMBO:
      return "BiquadCombo";
    case FILTER_TYPE_DIFF_EQ:
      return "DiffEq";
    case FILTER_TYPE_DITHER:
      return "Dither";
    case FILTER_TYPE_LIMITER:
      return "Limiter";
    case FILTER_TYPE_LOOKAHEAD_LIMITER:
      return "LookaheadLimiter";
    default:
      return "Gain";
  }
}

filter_type_t filter_type_from_string(const char* str) {
  if (!str) return FILTER_TYPE_GAIN;
  if (strcmp(str, "Gain") == 0) return FILTER_TYPE_GAIN;
  if (strcmp(str, "Volume") == 0) return FILTER_TYPE_VOLUME;
  if (strcmp(str, "Loudness") == 0) return FILTER_TYPE_LOUDNESS;
  if (strcmp(str, "Biquad") == 0) return FILTER_TYPE_BIQUAD;
  if (strcmp(str, "Conv") == 0) return FILTER_TYPE_CONV;
  if (strcmp(str, "Delay") == 0) return FILTER_TYPE_DELAY;
  if (strcmp(str, "BiquadCombo") == 0) return FILTER_TYPE_BIQUAD_COMBO;
  if (strcmp(str, "DiffEq") == 0) return FILTER_TYPE_DIFF_EQ;
  if (strcmp(str, "Dither") == 0) return FILTER_TYPE_DITHER;
  if (strcmp(str, "Limiter") == 0) return FILTER_TYPE_LIMITER;
  if (strcmp(str, "LookaheadLimiter") == 0)
    return FILTER_TYPE_LOOKAHEAD_LIMITER;
  return FILTER_TYPE_GAIN;
}

int gain_parameters_validate(const gain_parameters_t* params,
                             config_error_t* err) {
  if (!params) return 0;
  if (params->has_gain) {
    if (params->gain < -150.0 || params->gain > 150.0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "gain must be in [-150, 150] dB, got %g", params->gain);
      return -1;
    }
  }
  return 0;
}

int loudness_parameters_validate(const loudness_parameters_t* params,
                                 config_error_t* err) {
  if (!params) return 0;
  if (params->has_reference_level) {
    if (params->reference_level < -100.0 || params->reference_level > 20.0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "reference_level must be in [-100, 20], got %g",
                       params->reference_level);
      return -1;
    }
  }
  if (params->has_high_boost) {
    if (params->high_boost < 0.0 || params->high_boost > 20.0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "high_boost must be in [0, 20], got %g",
                       params->high_boost);
      return -1;
    }
  }
  if (params->has_low_boost) {
    if (params->low_boost < 0.0 || params->low_boost > 20.0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "low_boost must be in [0, 20], got %g",
                       params->low_boost);
      return -1;
    }
  }
  return 0;
}

int conv_parameters_validate(const conv_parameters_t* params,
                             config_error_t* err) {
  if (!params) return 0;
  switch (params->type) {
    case CONV_TYPE_VALUES:
      if (!params->values || params->values_count == 0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Conv 'values' must be non-empty");
        return -1;
      }
      break;
    case CONV_TYPE_WAV:
      if (params->filename[0] == '\0') {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Conv 'Wav' missing filename");
        return -1;
      }
      break;
    case CONV_TYPE_RAW:
      if (params->filename[0] == '\0') {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Conv 'Raw' missing filename");
        return -1;
      }
      break;
    case CONV_TYPE_DUMMY:
      if (params->length <= 0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Conv 'dummy' length must be > 0");
        return -1;
      }
      break;
  }
  return 0;
}

int delay_parameters_validate(const delay_parameters_t* params,
                              config_error_t* err) {
  if (!params) return 0;
  if (params->delay < 0.0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Delay cannot be negative, got %g", params->delay);
    return -1;
  }
  return 0;
}

int biquad_combo_parameters_validate(const biquad_combo_parameters_t* params,
                                     int sample_rate, config_error_t* err) {
  if (!params) return 0;
  double nyquist = (double)sample_rate / 2.0;
  switch (params->type) {
    case BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS:
    case BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS:
      if (!params->has_freq || params->freq <= 0.0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "BiquadCombo: freq must be > 0, got %g", params->freq);
        return -1;
      }
      if (params->freq >= nyquist) {
        config_error_set(
            err, CONFIG_ERR_INVALID_FILTER,
            "BiquadCombo: freq must be less than Nyquist (%g), got %g", nyquist,
            params->freq);
        return -1;
      }
      if (!params->has_order || params->order <= 0 || params->order > 64) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "BiquadCombo: order must be between 1 and 64, got %d",
                         params->order);
        return -1;
      }
      break;
    case BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS:
    case BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS:
      if (!params->has_freq || params->freq <= 0.0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "BiquadCombo: freq must be > 0, got %g", params->freq);
        return -1;
      }
      if (params->freq >= nyquist) {
        config_error_set(
            err, CONFIG_ERR_INVALID_FILTER,
            "BiquadCombo: freq must be less than Nyquist (%g), got %g", nyquist,
            params->freq);
        return -1;
      }
      if (!params->has_order || params->order <= 0 || params->order > 64 ||
          (params->order % 2) != 0) {
        config_error_set(
            err, CONFIG_ERR_INVALID_FILTER,
            "Linkwitz-Riley order must be an even number between 2 and 64, got %d",
            params->order);
        return -1;
      }
      break;
    case BIQUAD_COMBO_TYPE_TILT:
      if (!params->has_gain) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Tilt: gain must be set");
        return -1;
      }
      if (params->gain <= -100.0 || params->gain >= 100.0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Tilt: gain must be between -100 and 100 dB, got %g",
                         params->gain);
        return -1;
      }
      break;
    case BIQUAD_COMBO_TYPE_FIVE_POINT_PEQ:
      if (!params->has_qls || params->qls <= 0.0 || !params->has_qhs ||
          params->qhs <= 0.0 || !params->has_qp1 || params->qp1 <= 0.0 ||
          !params->has_qp2 || params->qp2 <= 0.0 || !params->has_qp3 ||
          params->qp3 <= 0.0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "FivePointPeq: all Q-values must be > 0");
        return -1;
      }
      if (!params->has_fls || params->fls >= nyquist || !params->has_fhs ||
          params->fhs >= nyquist || !params->has_fp1 ||
          params->fp1 >= nyquist || !params->has_fp2 ||
          params->fp2 >= nyquist || !params->has_fp3 ||
          params->fp3 >= nyquist) {
        config_error_set(
            err, CONFIG_ERR_INVALID_FILTER,
            "FivePointPeq: all frequencies must be less than Nyquist (%g)",
            nyquist);
        return -1;
      }
      if (params->fls <= 0.0 || params->fhs <= 0.0 || params->fp1 <= 0.0 ||
          params->fp2 <= 0.0 || params->fp3 <= 0.0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "FivePointPeq: all frequencies must be > 0");
        return -1;
      }
      break;
    case BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER:
      if (!params->gains || params->gains_count <= 0 || params->gains_count > 32) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "GraphicEqualizer: gains must be non-empty and have at most 32 bands, got %d",
                         params->gains_count);
        return -1;
      }
      if (!params->has_freq_min || params->freq_min <= 0.0 ||
          !params->has_freq_max || params->freq_max <= 0.0) {
        config_error_set(
            err, CONFIG_ERR_INVALID_FILTER,
            "GraphicEqualizer: min and max frequencies must be > 0");
        return -1;
      }
      if (params->freq_min >= nyquist || params->freq_max >= nyquist) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "GraphicEqualizer: min and max frequencies must be "
                         "less than Nyquist (%g)",
                         nyquist);
        return -1;
      }
      if (params->freq_min >= params->freq_max) {
        config_error_set(
            err, CONFIG_ERR_INVALID_FILTER,
            "GraphicEqualizer: min frequency must be lower than max frequency");
        return -1;
      }
      for (size_t i = 0; i < params->gains_count; i++) {
        double g = params->gains[i];
        if (g < -40.0 || g > 40.0) {
          config_error_set(
              err, CONFIG_ERR_INVALID_FILTER,
              "GraphicEqualizer: gains must be within +- 40 dB, got %g", g);
          return -1;
        }
      }
      break;
  }
  return 0;
}

int diff_eq_parameters_validate(const diff_eq_parameters_t* params,
                                config_error_t* err) {
  (void)params;
  (void)err;
  return 0;
}

int dither_parameters_validate(const dither_parameters_t* params,
                               config_error_t* err) {
  if (!params) return 0;
  if (params->bits < 2) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Dither bit depth must be at least 2, got %d",
                     params->bits);
    return -1;
  }
  if (params->has_amplitude) {
    if (params->amplitude < 0.0 || params->amplitude > 100.0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Dither amplitude must be in [0, 100], got %g",
                       params->amplitude);
      return -1;
    }
  }
  return 0;
}

int limiter_parameters_validate(const limiter_parameters_t* params,
                                config_error_t* err) {
  if (!params) return 0;
  if (!isfinite(params->clip_limit) || params->clip_limit < -120.0 || params->clip_limit > 20.0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Limiter clip_limit must be between -120.0 dB and 20.0 dB, got %g",
                     params->clip_limit);
    return -1;
  }
  return 0;
}

int lookahead_limiter_parameters_validate(
    const lookahead_limiter_parameters_t* params, int sample_rate,
    config_error_t* err) {
  if (sample_rate <= 0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Lookahead Limiter: sample_rate must be greater than 0, got %d",
                     sample_rate);
    return -1;
  }
  if (!params) return 0;
  if (!isfinite(params->limit) || params->limit < -120.0 || params->limit > 20.0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Lookahead Limiter limit must be between -120.0 dB and 20.0 dB, got %g",
                     params->limit);
    return -1;
  }
  if (params->attack < 0.0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Lookahead Limiter: attack cannot be negative, got %g",
                     params->attack);
    return -1;
  }
  if (params->release < 0.0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Lookahead Limiter: release cannot be negative, got %g",
                     params->release);
    return -1;
  }
  double attack_samples = 0.0;
  switch (params->unit) {
    case DELAY_UNIT_MS:
      attack_samples = params->attack / 1000.0 * (double)sample_rate;
      break;
    case DELAY_UNIT_US:
      attack_samples = params->attack / 1000000.0 * (double)sample_rate;
      break;
    case DELAY_UNIT_SAMPLES:
      attack_samples = params->attack;
      break;
    case DELAY_UNIT_MM:
      attack_samples = params->attack / 1000.0 * (double)sample_rate / 343.0;
      break;
  }
  if (attack_samples > (double)sample_rate) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Lookahead Limiter: attack time cannot be longer than 1 "
                     "second, got %g samples",
                     attack_samples);
    return -1;
  }
  return 0;
}

int volume_parameters_validate(const volume_parameters_t* params,
                               config_error_t* err) {
  if (!params) return 0;
  if (params->has_ramp_time) {
    if (params->ramp_time < 0.0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Volume ramp time cannot be negative, got %g",
                       params->ramp_time);
      return -1;
    }
  }
  return 0;
}

int filter_config_validate(const filter_config_t* filter, int sample_rate,
                           config_error_t* err) {
  if (!filter) return 0;
  switch (filter->type) {
    case FILTER_TYPE_GAIN:
      return gain_parameters_validate(&filter->parameters.gain, err);
    case FILTER_TYPE_VOLUME:
      return volume_parameters_validate(&filter->parameters.volume, err);
    case FILTER_TYPE_LOUDNESS:
      return loudness_parameters_validate(&filter->parameters.loudness, err);
    case FILTER_TYPE_BIQUAD:
      return biquad_parameters_validate(&filter->parameters.biquad, sample_rate,
                                        err);
    case FILTER_TYPE_CONV:
      return conv_parameters_validate(&filter->parameters.conv, err);
    case FILTER_TYPE_DELAY:
      return delay_parameters_validate(&filter->parameters.delay, err);
    case FILTER_TYPE_BIQUAD_COMBO:
      return biquad_combo_parameters_validate(&filter->parameters.biquad_combo,
                                              sample_rate, err);
    case FILTER_TYPE_DIFF_EQ:
      return diff_eq_parameters_validate(&filter->parameters.diff_eq, err);
    case FILTER_TYPE_DITHER:
      return dither_parameters_validate(&filter->parameters.dither, err);
    case FILTER_TYPE_LIMITER:
      return limiter_parameters_validate(&filter->parameters.limiter, err);
    case FILTER_TYPE_LOOKAHEAD_LIMITER:
      return lookahead_limiter_parameters_validate(
          &filter->parameters.lookahead_limiter, sample_rate, err);
  }
  return 0;
}

int biquad_parameters_validate(const biquad_parameters_t* params,
                               int sample_rate, config_error_t* err) {
  if (!params) return -1;
  double nyquist = (double)sample_rate / 2.0;
  if (params->type != BIQUAD_TYPE_FREE &&
      params->type != BIQUAD_TYPE_LINKWITZ_TRANSFORM &&
      params->type != BIQUAD_TYPE_GENERAL_NOTCH) {
    if (params->freq <= 0.0 || params->freq >= nyquist) {
      if (err)
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "freq out of range");
      return -1;
    }
  }
  if (params->type == BIQUAD_TYPE_GENERAL_NOTCH) {
    if (params->freq_notch <= 0.0 || params->freq_notch >= nyquist ||
        params->freq_pole <= 0.0 || params->freq_pole >= nyquist) {
      if (err)
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "freq out of range");
      return -1;
    }
    if (params->q_p <= 0.0) {
      if (err)
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "q out of range");
      return -1;
    }
  }
  if (params->type == BIQUAD_TYPE_LINKWITZ_TRANSFORM) {
    if (params->freq_act <= 0.0 || params->freq_act >= nyquist ||
        params->freq_target <= 0.0 || params->freq_target >= nyquist) {
      if (err)
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "freq out of range");
      return -1;
    }
    if (params->q_act <= 0.0 || params->q_target <= 0.0) {
      if (err)
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "q out of range");
      return -1;
    }
  }
  if (params->type == BIQUAD_TYPE_PEAKING ||
      params->type == BIQUAD_TYPE_LOWPASS ||
      params->type == BIQUAD_TYPE_HIGHPASS ||
      params->type == BIQUAD_TYPE_BANDPASS ||
      params->type == BIQUAD_TYPE_NOTCH ||
      params->type == BIQUAD_TYPE_ALLPASS ||
      params->type == BIQUAD_TYPE_GENERAL_NOTCH ||
      params->type == BIQUAD_TYPE_HIGHSHELF ||
      params->type == BIQUAD_TYPE_LOWSHELF) {
    if (params->q <= 0.0 && params->bandwidth <= 0.0 && params->slope <= 0.0) {
      if (err)
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "q out of range");
      return -1;
    }
  }
  if (params->type == BIQUAD_TYPE_HIGHSHELF ||
      params->type == BIQUAD_TYPE_LOWSHELF) {
    if (params->steepness_type == STEEPNESS_TYPE_SLOPE) {
      if (params->slope <= 0.0 || params->slope > 12.0) {
        if (err)
          config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                           "slope out of range");
        return -1;
      }
    }
  }
  // Stability check: pole positions of the realised coefficients must
  // lie strictly inside the unit circle.
  biquad_coefficients_t coeffs;
  if (biquad_coefficients_compute(params, sample_rate, &coeffs)) {
    /* Verify filter stability using the stability triangle criteria for a
     * second-order IIR filter. The poles of the transfer function H(z) must
     * reside inside the unit circle. This requires:
     *   1. |a2| < 1
     *   2. |a1| < 1 + a2
     * If these are violated, the filter is unstable. */
    if (fabs(coeffs.a2) >= 1.0 || fabs(coeffs.a1) >= 1.0 + coeffs.a2) {
      if (err)
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "unstable biquad");
      return -1;
    }
  } else {
    return -1;
  }
  return 0;
}

double compute_delay_samples(double delay, delay_unit_t unit, int sample_rate) {
  switch (unit) {
    case DELAY_UNIT_MS:
      return delay / 1000.0 * (double)sample_rate;
    case DELAY_UNIT_US:
      return delay / 1000000.0 * (double)sample_rate;
    case DELAY_UNIT_SAMPLES:
      return delay;
    case DELAY_UNIT_MM:
      // Compute delay using speed of sound in air (approx. 343 m/s)
      return delay / 1000.0 * (double)sample_rate / 343.0;
    default:
      return delay;
  }
}
