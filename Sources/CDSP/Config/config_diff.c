#include "config_diff.h"

struct config_change {
  config_change_type_t type;
  char** filters;
  size_t filters_count;
  char** mixers;
  size_t mixers_count;
  char** processors;
  size_t processors_count;
};

#include <stdlib.h>
#include <string.h>

/**
 * @brief Compares two double arrays for equality.
 * @param a First double array.
 * @param a_count Number of elements in first array.
 * @param b Second double array.
 * @param b_count Number of elements in second array.
 * @return true if arrays are equal in size and elements, false otherwise.
 */
static bool double_arrays_equal(const double* a, size_t a_count,
                                const double* b, size_t b_count) {
  if (a_count != b_count) return false;
  if (a_count == 0) return true;
  if (!a || !b) return false;
  for (size_t i = 0; i < a_count; i++) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

/**
 * @brief Compares two integer arrays for equality.
 * @param a First integer array.
 * @param a_count Number of elements in first array.
 * @param b Second integer array.
 * @param b_count Number of elements in second array.
 * @return true if arrays are equal in size and elements, false otherwise.
 */
static bool int_arrays_equal(const int* a, size_t a_count, const int* b,
                             size_t b_count) {
  if (a_count != b_count) return false;
  if (a_count == 0) return true;
  if (!a || !b) return false;
  for (size_t i = 0; i < a_count; i++) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

/**
 * @brief Compares two string arrays for equality.
 * @param a First string array.
 * @param a_count Number of elements in first array.
 * @param b Second string array.
 * @param b_count Number of elements in second array.
 * @return true if arrays are equal in size and strings, false otherwise.
 */
static bool string_arrays_equal(char** a, size_t a_count, char** b,
                                size_t b_count) {
  if (a_count != b_count) return false;
  if (a_count == 0) return true;
  if (!a || !b) return false;
  for (size_t i = 0; i < a_count; i++) {
    if ((a[i] == NULL) != (b[i] == NULL)) return false;
    if (a[i] && strcmp(a[i], b[i]) != 0) return false;
  }
  return true;
}

/**
 * @brief Compares two biquad parameter structures for equality.
 * @param a Pointer to first biquad parameters.
 * @param b Pointer to second biquad parameters.
 * @return true if parameters are equal, false otherwise.
 */
static bool biquad_parameters_equal(const biquad_parameters_t* a,
                                    const biquad_parameters_t* b) {
  if (a->type != b->type) return false;
  if (a->freq != b->freq) return false;
  if (a->gain != b->gain) return false;
  if (a->q != b->q) return false;
  if (a->bandwidth != b->bandwidth) return false;
  if (a->slope != b->slope) return false;
  if (a->a1 != b->a1 || a->a2 != b->a2 || a->b0 != b->b0 || a->b1 != b->b1 ||
      a->b2 != b->b2)
    return false;
  if (a->freq_notch != b->freq_notch || a->freq_pole != b->freq_pole ||
      a->q_p != b->q_p)
    return false;
  if (a->normalize_at_dc != b->normalize_at_dc) return false;
  if (a->freq_act != b->freq_act || a->q_act != b->q_act ||
      a->freq_target != b->freq_target || a->q_target != b->q_target)
    return false;
  if (a->steepness_type != b->steepness_type) return false;
  return true;
}

/**
 * @brief Compares two volume parameter structures for equality.
 * @param a Pointer to first volume parameters.
 * @param b Pointer to second volume parameters.
 * @return true if parameters are equal, false otherwise.
 */
static bool volume_parameters_equal(const volume_parameters_t* a,
                                    const volume_parameters_t* b) {
  if (a->ramp_time != b->ramp_time) return false;
  if (a->has_ramp_time != b->has_ramp_time) return false;
  if (a->limit != b->limit) return false;
  if (a->has_limit != b->has_limit) return false;
  if (a->fader != b->fader) return false;
  return true;
}

/**
 * @brief Compares two loudness parameter structures for equality.
 * @param a Pointer to first loudness parameters.
 * @param b Pointer to second loudness parameters.
 * @return true if parameters are equal, false otherwise.
 */
static bool loudness_parameters_equal(const loudness_parameters_t* a,
                                      const loudness_parameters_t* b) {
  if (a->reference_level != b->reference_level) return false;
  if (a->has_reference_level != b->has_reference_level) return false;
  if (a->high_boost != b->high_boost) return false;
  if (a->has_high_boost != b->has_high_boost) return false;
  if (a->low_boost != b->low_boost) return false;
  if (a->has_low_boost != b->has_low_boost) return false;
  if (a->attenuate_mid != b->attenuate_mid) return false;
  if (a->fader != b->fader) return false;
  return true;
}

/**
 * @brief Compares two convolution parameter structures for equality.
 * @param a Pointer to first convolution parameters.
 * @param b Pointer to second convolution parameters.
 * @return true if parameters are equal, false otherwise.
 */
static bool conv_parameters_equal(const conv_parameters_t* a,
                                  const conv_parameters_t* b) {
  if (a->type != b->type) return false;
  if (strcmp(a->filename, b->filename) != 0) return false;
  if (strcmp(a->format, b->format) != 0) return false;
  if (a->channel != b->channel) return false;
  if (a->length != b->length) return false;
  if (a->skip_bytes_lines != b->skip_bytes_lines) return false;
  if (a->read_bytes_lines != b->read_bytes_lines) return false;
  if (!double_arrays_equal(a->values, a->values_count, b->values,
                           b->values_count))
    return false;
  return true;
}

/**
 * @brief Compares two delay parameter structures for equality.
 * @param a Pointer to first delay parameters.
 * @param b Pointer to second delay parameters.
 * @return true if parameters are equal, false otherwise.
 */
static bool delay_parameters_equal(const delay_parameters_t* a,
                                   const delay_parameters_t* b) {
  if (a->delay != b->delay) return false;
  if (a->unit != b->unit) return false;
  if (a->subsample != b->subsample) return false;
  return true;
}

/**
 * @brief Compares two biquad combo parameter structures for equality.
 * @param a Pointer to first biquad combo parameters.
 * @param b Pointer to second biquad combo parameters.
 * @return true if parameters are equal, false otherwise.
 */
static bool biquad_combo_parameters_equal(const biquad_combo_parameters_t* a,
                                          const biquad_combo_parameters_t* b) {
  if (a->type != b->type) return false;
  if (a->freq != b->freq) return false;
  if (a->has_freq != b->has_freq) return false;
  if (a->order != b->order) return false;
  if (a->has_order != b->has_order) return false;
  if (a->gain != b->gain) return false;
  if (a->has_gain != b->has_gain) return false;
  if (a->fls != b->fls || a->qls != b->qls || a->gls != b->gls) return false;
  if (a->has_fls != b->has_fls || a->has_qls != b->has_qls ||
      a->has_gls != b->has_gls)
    return false;
  if (a->fp1 != b->fp1 || a->qp1 != b->qp1 || a->gp1 != b->gp1) return false;
  if (a->has_fp1 != b->has_fp1 || a->has_qp1 != b->has_qp1 ||
      a->has_gp1 != b->has_gp1)
    return false;
  if (a->fp2 != b->fp2 || a->qp2 != b->qp2 || a->gp2 != b->gp2) return false;
  if (a->has_fp2 != b->has_fp2 || a->has_qp2 != b->has_qp2 ||
      a->has_gp2 != b->has_gp2)
    return false;
  if (a->fp3 != b->fp3 || a->qp3 != b->qp3 || a->gp3 != b->gp3) return false;
  if (a->has_fp3 != b->has_fp3 || a->has_qp3 != b->has_qp3 ||
      a->has_gp3 != b->has_gp3)
    return false;
  if (a->fhs != b->fhs || a->qhs != b->qhs || a->ghs != b->ghs) return false;
  if (a->has_fhs != b->has_fhs || a->has_qhs != b->has_qhs ||
      a->has_ghs != b->has_ghs)
    return false;
  if (a->freq_min != b->freq_min || a->freq_max != b->freq_max) return false;
  if (a->has_freq_min != b->has_freq_min || a->has_freq_max != b->has_freq_max)
    return false;
  if (!double_arrays_equal(a->gains, a->gains_count, b->gains, b->gains_count))
    return false;
  return true;
}

/**
 * @brief Compares two difference equation parameter structures for equality.
 * @param a Pointer to first difference equation parameters.
 * @param b Pointer to second difference equation parameters.
 * @return true if parameters are equal, false otherwise.
 */
static bool diff_eq_parameters_equal(const diff_eq_parameters_t* a,
                                     const diff_eq_parameters_t* b) {
  if (!double_arrays_equal(a->a, a->a_count, b->a, b->a_count)) return false;
  if (!double_arrays_equal(a->b, a->b_count, b->b, b->b_count)) return false;
  return true;
}

/**
 * @brief Compares two dither parameter structures for equality.
 * @param a Pointer to first dither parameters.
 * @param b Pointer to second dither parameters.
 * @return true if parameters are equal, false otherwise.
 */
static bool dither_parameters_equal(const dither_parameters_t* a,
                                    const dither_parameters_t* b) {
  if (a->type != b->type) return false;
  if (a->bits != b->bits) return false;
  if (a->amplitude != b->amplitude) return false;
  if (a->has_amplitude != b->has_amplitude) return false;
  return true;
}

/**
 * @brief Compares two limiter parameter structures for equality.
 * @param a Pointer to first limiter parameters.
 * @param b Pointer to second limiter parameters.
 * @return true if parameters are equal, false otherwise.
 */
static bool limiter_parameters_equal(const limiter_parameters_t* a,
                                     const limiter_parameters_t* b) {
  if (a->clip_limit != b->clip_limit) return false;
  if (a->soft_clip != b->soft_clip) return false;
  return true;
}

/**
 * @brief Compares two lookahead limiter parameter structures for equality.
 * @param a Pointer to first lookahead limiter parameters.
 * @param b Pointer to second lookahead limiter parameters.
 * @return true if parameters are equal, false otherwise.
 */
static bool lookahead_limiter_parameters_equal(
    const lookahead_limiter_parameters_t* a,
    const lookahead_limiter_parameters_t* b) {
  if (a->limit != b->limit) return false;
  if (a->attack != b->attack) return false;
  if (a->release != b->release) return false;
  if (a->unit != b->unit) return false;
  return true;
}

/**
 * @brief Compares two filter configurations for equality.
 * @param a Pointer to first filter configuration.
 * @param b Pointer to second filter configuration.
 * @return true if configurations are equal, false otherwise.
 */
static bool filter_config_equal(const filter_config_t* a,
                                const filter_config_t* b) {
  if (a->type != b->type) return false;
  switch (a->type) {
    case FILTER_TYPE_GAIN:
      return a->parameters.gain.gain == b->parameters.gain.gain &&
             a->parameters.gain.inverted == b->parameters.gain.inverted;
    case FILTER_TYPE_VOLUME:
      return volume_parameters_equal(&a->parameters.volume,
                                     &b->parameters.volume);
    case FILTER_TYPE_LOUDNESS:
      return loudness_parameters_equal(&a->parameters.loudness,
                                       &b->parameters.loudness);
    case FILTER_TYPE_BIQUAD:
      return biquad_parameters_equal(&a->parameters.biquad,
                                     &b->parameters.biquad);
    case FILTER_TYPE_CONV:
      return conv_parameters_equal(&a->parameters.conv, &b->parameters.conv);
    case FILTER_TYPE_DELAY:
      return delay_parameters_equal(&a->parameters.delay, &b->parameters.delay);
    case FILTER_TYPE_BIQUAD_COMBO:
      return biquad_combo_parameters_equal(&a->parameters.biquad_combo,
                                           &b->parameters.biquad_combo);
    case FILTER_TYPE_DIFF_EQ:
      return diff_eq_parameters_equal(&a->parameters.diff_eq,
                                      &b->parameters.diff_eq);
    case FILTER_TYPE_DITHER:
      return dither_parameters_equal(&a->parameters.dither,
                                     &b->parameters.dither);
    case FILTER_TYPE_LIMITER:
      return limiter_parameters_equal(&a->parameters.limiter,
                                      &b->parameters.limiter);
    case FILTER_TYPE_LOOKAHEAD_LIMITER:
      return lookahead_limiter_parameters_equal(
          &a->parameters.lookahead_limiter, &b->parameters.lookahead_limiter);
    default:
      return false;
  }
}

/**
 * @brief Compares two mixer configurations for equality.
 * @param a Pointer to first mixer configuration.
 * @param b Pointer to second mixer configuration.
 * @return true if configurations are equal, false otherwise.
 */
static bool mixer_config_equal(const mixer_config_t* a,
                               const mixer_config_t* b) {
  if (a->channels_in != b->channels_in) return false;
  if (a->channels_out != b->channels_out) return false;
  if (a->mapping_count != b->mapping_count) return false;
  for (size_t i = 0; i < a->mapping_count; i++) {
    const mixer_mapping_t* ma = &a->mappings[i];
    const mixer_mapping_t* mb = &b->mappings[i];
    if (ma->dest != mb->dest) return false;
    if (ma->sources_count != mb->sources_count) return false;
    for (size_t j = 0; j < ma->sources_count; j++) {
      const mixer_source_t* sa = &ma->sources[j];
      const mixer_source_t* sb = &mb->sources[j];
      if (sa->channel != sb->channel) return false;
      if (sa->gain != sb->gain) return false;
      if (sa->inverted != sb->inverted) return false;
      if (sa->scale != sb->scale) return false;
      if (sa->mute != sb->mute) return false;
    }
  }
  return true;
}

/**
 * @brief Compares two processor configurations for equality.
 * @param a Pointer to first processor configuration.
 * @param b Pointer to second processor configuration.
 * @return true if configurations are equal, false otherwise.
 */
static bool processor_config_equal(const processor_config_t* a,
                                   const processor_config_t* b) {
  if (a->type != b->type) return false;
  switch (a->type) {
    case PROCESSOR_TYPE_COMPRESSOR:
      if (a->parameters.compressor.channels !=
          b->parameters.compressor.channels)
        return false;
      if (!int_arrays_equal(a->parameters.compressor.monitor_channels,
                            a->parameters.compressor.monitor_channels_count,
                            b->parameters.compressor.monitor_channels,
                            b->parameters.compressor.monitor_channels_count))
        return false;
      if (!int_arrays_equal(a->parameters.compressor.process_channels,
                            a->parameters.compressor.process_channels_count,
                            b->parameters.compressor.process_channels,
                            b->parameters.compressor.process_channels_count))
        return false;
      if (a->parameters.compressor.attack != b->parameters.compressor.attack ||
          a->parameters.compressor.release !=
              b->parameters.compressor.release ||
          a->parameters.compressor.threshold !=
              b->parameters.compressor.threshold ||
          a->parameters.compressor.factor != b->parameters.compressor.factor)
        return false;
      if (a->parameters.compressor.makeup_gain !=
              b->parameters.compressor.makeup_gain ||
          a->parameters.compressor.has_makeup_gain !=
              b->parameters.compressor.has_makeup_gain)
        return false;
      if (a->parameters.compressor.soft_clip !=
              b->parameters.compressor.soft_clip ||
          a->parameters.compressor.clip_limit !=
              b->parameters.compressor.clip_limit ||
          a->parameters.compressor.has_clip_limit !=
              b->parameters.compressor.has_clip_limit)
        return false;
      return true;

    case PROCESSOR_TYPE_NOISE_GATE:
      if (a->parameters.noise_gate.channels !=
          b->parameters.noise_gate.channels)
        return false;
      if (!int_arrays_equal(a->parameters.noise_gate.monitor_channels,
                            a->parameters.noise_gate.monitor_channels_count,
                            b->parameters.noise_gate.monitor_channels,
                            b->parameters.noise_gate.monitor_channels_count))
        return false;
      if (!int_arrays_equal(a->parameters.noise_gate.process_channels,
                            a->parameters.noise_gate.process_channels_count,
                            b->parameters.noise_gate.process_channels,
                            b->parameters.noise_gate.process_channels_count))
        return false;
      if (a->parameters.noise_gate.attack != b->parameters.noise_gate.attack ||
          a->parameters.noise_gate.release !=
              b->parameters.noise_gate.release ||
          a->parameters.noise_gate.threshold !=
              b->parameters.noise_gate.threshold ||
          a->parameters.noise_gate.attenuation !=
              b->parameters.noise_gate.attenuation)
        return false;
      return true;

    case PROCESSOR_TYPE_RACE:
      if (a->parameters.race.channels != b->parameters.race.channels ||
          a->parameters.race.channel_a != b->parameters.race.channel_a ||
          a->parameters.race.channel_b != b->parameters.race.channel_b ||
          a->parameters.race.delay != b->parameters.race.delay ||
          a->parameters.race.subsample_delay !=
              b->parameters.race.subsample_delay ||
          a->parameters.race.has_subsample_delay !=
              b->parameters.race.has_subsample_delay ||
          a->parameters.race.delay_unit != b->parameters.race.delay_unit ||
          a->parameters.race.has_delay_unit !=
              b->parameters.race.has_delay_unit ||
          a->parameters.race.attenuation != b->parameters.race.attenuation)
        return false;
      return true;

    default:
      return false;
  }
}

/**
 * @brief Compares two resampler configurations for equality.
 * @param a Pointer to first resampler configuration.
 * @param b Pointer to second resampler configuration.
 * @return true if configurations are equal, false otherwise.
 */
static bool resampler_config_equal(const resampler_config_t* a,
                                   const resampler_config_t* b) {
  if (a->type != b->type) return false;
  if (a->has_profile != b->has_profile) return false;
  if (a->has_profile && strcmp(a->profile, b->profile) != 0) return false;
  if (a->has_interpolation != b->has_interpolation) return false;
  if (a->has_interpolation && strcmp(a->interpolation, b->interpolation) != 0)
    return false;
  if (a->has_sinc_len != b->has_sinc_len) return false;
  if (a->has_sinc_len && a->sinc_len != b->sinc_len) return false;
  if (a->has_oversampling_factor != b->has_oversampling_factor) return false;
  if (a->has_oversampling_factor &&
      a->oversampling_factor != b->oversampling_factor)
    return false;
  if (a->has_window != b->has_window) return false;
  if (a->has_window && strcmp(a->window, b->window) != 0) return false;
  if (a->has_f_cutoff != b->has_f_cutoff) return false;
  if (a->has_f_cutoff && a->f_cutoff != b->f_cutoff) return false;

#if defined(ENABLE_COREAUDIO)
  if (a->has_apple_quality != b->has_apple_quality) return false;
  if (a->has_apple_quality && a->apple_quality != b->apple_quality)
    return false;
  if (a->has_apple_complexity != b->has_apple_complexity) return false;
  if (a->has_apple_complexity && a->apple_complexity != b->apple_complexity)
    return false;
#endif

  return true;
}

/**
 * @brief Compares two devices configurations for equality.
 * @param a Pointer to first devices configuration.
 * @param b Pointer to second devices configuration.
 * @return true if configurations are equal, false otherwise.
 */
bool devices_config_equal(const devices_config_t* a,
                           const devices_config_t* b) {
  if (a->samplerate != b->samplerate) return false;
  if (a->chunksize != b->chunksize) return false;
  if (a->enable_rate_adjust != b->enable_rate_adjust) return false;
  if (a->has_enable_rate_adjust != b->has_enable_rate_adjust) return false;
  if (a->target_level != b->target_level) return false;
  if (a->has_target_level != b->has_target_level) return false;
  if (a->adjust_period != b->adjust_period) return false;
  if (a->has_adjust_period != b->has_adjust_period) return false;
  if (a->capture_samplerate != b->capture_samplerate) return false;
  if (a->has_capture_samplerate != b->has_capture_samplerate) return false;
  if (a->silence_threshold != b->silence_threshold) return false;
  if (a->has_silence_threshold != b->has_silence_threshold) return false;
  if (a->silence_timeout != b->silence_timeout) return false;
  if (a->has_silence_timeout != b->has_silence_timeout) return false;
  if (a->volume_ramp_time != b->volume_ramp_time) return false;
  if (a->has_volume_ramp_time != b->has_volume_ramp_time) return false;
  if (a->volume_limit != b->volume_limit) return false;
  if (a->has_volume_limit != b->has_volume_limit) return false;
  if (a->queuelimit != b->queuelimit) return false;
  if (a->has_queuelimit != b->has_queuelimit) return false;

  if (a->has_resampler != b->has_resampler) return false;
  if (a->has_resampler) {
    if (!resampler_config_equal(&a->resampler, &b->resampler)) return false;
  }

  if (a->capture.type != b->capture.type) return false;
  if (a->capture.is_wav != b->capture.is_wav) return false;
  if (a->capture.has_is_wav != b->capture.has_is_wav) return false;
  if (a->capture.bypass_dop != b->capture.bypass_dop) return false;
  if (a->capture.has_bypass_dop != b->capture.has_bypass_dop) return false;
  if (a->capture.dop_cutoff_hz != b->capture.dop_cutoff_hz) return false;
  if (a->capture.has_dop_cutoff_hz != b->capture.has_dop_cutoff_hz) return false;

  if (a->capture.has_labels != b->capture.has_labels) return false;
  if (a->capture.has_labels) {
    if (a->capture.labels_count != b->capture.labels_count) return false;
    for (size_t i = 0; i < a->capture.labels_count; i++) {
      if (strcmp(a->capture.labels[i], b->capture.labels[i]) != 0) return false;
    }
  }

  switch (a->capture.type) {
#if defined(ENABLE_COREAUDIO)
    case AUDIO_BACKEND_TYPE_CORE_AUDIO:
      if (a->capture.cfg.coreaudio.channels != b->capture.cfg.coreaudio.channels) return false;
      if (strcmp(a->capture.cfg.coreaudio.device, b->capture.cfg.coreaudio.device) != 0) return false;
      if (a->capture.cfg.coreaudio.format != b->capture.cfg.coreaudio.format) return false;
      break;
#endif
#if defined(ENABLE_ALSA)
    case AUDIO_BACKEND_TYPE_ALSA:
      if (a->capture.cfg.alsa.channels != b->capture.cfg.alsa.channels) return false;
      if (strcmp(a->capture.cfg.alsa.device, b->capture.cfg.alsa.device) != 0) return false;
      if (a->capture.cfg.alsa.format != b->capture.cfg.alsa.format) return false;
      if (a->capture.cfg.alsa.stop_on_inactive != b->capture.cfg.alsa.stop_on_inactive) return false;
      if (strcmp(a->capture.cfg.alsa.link_volume_control, b->capture.cfg.alsa.link_volume_control) != 0) return false;
      if (strcmp(a->capture.cfg.alsa.link_mute_control, b->capture.cfg.alsa.link_mute_control) != 0) return false;
      break;
#endif
#if defined(ENABLE_PULSE)
    case AUDIO_BACKEND_TYPE_PULSE_AUDIO:
      if (a->capture.cfg.pulse.channels != b->capture.cfg.pulse.channels) return false;
      if (strcmp(a->capture.cfg.pulse.device, b->capture.cfg.pulse.device) != 0) return false;
      break;
#endif
#if defined(ENABLE_PIPEWIRE)
    case AUDIO_BACKEND_TYPE_PIPEWIRE:
      if (a->capture.cfg.pipewire.channels != b->capture.cfg.pipewire.channels) return false;
      if (strcmp(a->capture.cfg.pipewire.device, b->capture.cfg.pipewire.device) != 0) return false;
      if (strcmp(a->capture.cfg.pipewire.node_name, b->capture.cfg.pipewire.node_name) != 0) return false;
      if (strcmp(a->capture.cfg.pipewire.node_description, b->capture.cfg.pipewire.node_description) != 0) return false;
      if (strcmp(a->capture.cfg.pipewire.node_group_name, b->capture.cfg.pipewire.node_group_name) != 0) return false;
      if (strcmp(a->capture.cfg.pipewire.autoconnect_to, b->capture.cfg.pipewire.autoconnect_to) != 0) return false;
      break;
#endif
#if defined(ENABLE_JACK)
    case AUDIO_BACKEND_TYPE_JACK:
      if (a->capture.cfg.jack.channels != b->capture.cfg.jack.channels) return false;
      if (strcmp(a->capture.cfg.jack.device, b->capture.cfg.jack.device) != 0) return false;
      break;
#endif
    case AUDIO_BACKEND_TYPE_FILE:
      if (a->capture.is_wav) {
        if (strcmp(a->capture.cfg.wav_file.filename, b->capture.cfg.wav_file.filename) != 0) return false;
        if (a->capture.cfg.wav_file.extra_samples != b->capture.cfg.wav_file.extra_samples) return false;
      } else {
        if (a->capture.cfg.raw_file.channels != b->capture.cfg.raw_file.channels) return false;
        if (strcmp(a->capture.cfg.raw_file.filename, b->capture.cfg.raw_file.filename) != 0) return false;
        if (a->capture.cfg.raw_file.format != b->capture.cfg.raw_file.format) return false;
        if (a->capture.cfg.raw_file.skip_bytes != b->capture.cfg.raw_file.skip_bytes) return false;
        if (a->capture.cfg.raw_file.read_bytes != b->capture.cfg.raw_file.read_bytes) return false;
        if (a->capture.cfg.raw_file.extra_samples != b->capture.cfg.raw_file.extra_samples) return false;
      }
      break;
    case AUDIO_BACKEND_TYPE_STDIN_OUT:
      if (a->capture.cfg.stdin_in.channels != b->capture.cfg.stdin_in.channels) return false;
      if (a->capture.cfg.stdin_in.format != b->capture.cfg.stdin_in.format) return false;
      if (a->capture.cfg.stdin_in.extra_samples != b->capture.cfg.stdin_in.extra_samples) return false;
      if (a->capture.cfg.stdin_in.skip_bytes != b->capture.cfg.stdin_in.skip_bytes) return false;
      if (a->capture.cfg.stdin_in.read_bytes != b->capture.cfg.stdin_in.read_bytes) return false;
      break;
    case AUDIO_BACKEND_TYPE_GENERATOR:
      if (a->capture.cfg.generator.channels != b->capture.cfg.generator.channels) return false;
      if (a->capture.cfg.generator.signal.type != b->capture.cfg.generator.signal.type) return false;
      if (a->capture.cfg.generator.signal.frequency != b->capture.cfg.generator.signal.frequency) return false;
      if (a->capture.cfg.generator.signal.level != b->capture.cfg.generator.signal.level) return false;
      break;
#if defined(ENABLE_WASAPI)
    case AUDIO_BACKEND_TYPE_WASAPI:
      if (a->capture.cfg.wasapi.channels != b->capture.cfg.wasapi.channels) return false;
      if (strcmp(a->capture.cfg.wasapi.device, b->capture.cfg.wasapi.device) != 0) return false;
      if (a->capture.cfg.wasapi.format != b->capture.cfg.wasapi.format) return false;
      if (a->capture.cfg.wasapi.exclusive != b->capture.cfg.wasapi.exclusive) return false;
      if (a->capture.cfg.wasapi.loopback != b->capture.cfg.wasapi.loopback) return false;
      if (a->capture.cfg.wasapi.polling != b->capture.cfg.wasapi.polling) return false;
      break;
#endif
#if defined(ENABLE_ASIO)
    case AUDIO_BACKEND_TYPE_ASIO:
      if (a->capture.cfg.asio.channels != b->capture.cfg.asio.channels) return false;
      if (strcmp(a->capture.cfg.asio.device, b->capture.cfg.asio.device) != 0) return false;
      if (a->capture.cfg.asio.format != b->capture.cfg.asio.format) return false;
      break;
#endif
#if defined(ENABLE_BLUEZ)
    case AUDIO_BACKEND_TYPE_BLUEZ:
      if (a->capture.cfg.bluez.channels != b->capture.cfg.bluez.channels) return false;
      if (strcmp(a->capture.cfg.bluez.service, b->capture.cfg.bluez.service) != 0) return false;
      break;
#endif
    default:
      break;
  }

  if (a->playback.type != b->playback.type) return false;
  if (a->playback.is_wav != b->playback.is_wav) return false;
  if (a->playback.has_is_wav != b->playback.has_is_wav) return false;
  if (a->playback.output_dop != b->playback.output_dop) return false;
  if (a->playback.has_output_dop != b->playback.has_output_dop) return false;
  if (a->playback.dop_encoder_filter != b->playback.dop_encoder_filter) return false;
  if (a->playback.has_dop_encoder_filter != b->playback.has_dop_encoder_filter) return false;

  if (a->playback.has_labels != b->playback.has_labels) return false;
  if (a->playback.has_labels) {
    if (a->playback.labels_count != b->playback.labels_count) return false;
    for (size_t i = 0; i < a->playback.labels_count; i++) {
      if (strcmp(a->playback.labels[i], b->playback.labels[i]) != 0) return false;
    }
  }

  switch (a->playback.type) {
#if defined(ENABLE_COREAUDIO)
    case AUDIO_BACKEND_TYPE_CORE_AUDIO:
      if (a->playback.cfg.coreaudio.channels != b->playback.cfg.coreaudio.channels) return false;
      if (strcmp(a->playback.cfg.coreaudio.device, b->playback.cfg.coreaudio.device) != 0) return false;
      if (a->playback.cfg.coreaudio.format != b->playback.cfg.coreaudio.format) return false;
      break;
#endif
#if defined(ENABLE_ALSA)
    case AUDIO_BACKEND_TYPE_ALSA:
      if (a->playback.cfg.alsa.channels != b->playback.cfg.alsa.channels) return false;
      if (strcmp(a->playback.cfg.alsa.device, b->playback.cfg.alsa.device) != 0) return false;
      if (a->playback.cfg.alsa.format != b->playback.cfg.alsa.format) return false;
      break;
#endif
#if defined(ENABLE_PULSE)
    case AUDIO_BACKEND_TYPE_PULSE_AUDIO:
      if (a->playback.cfg.pulse.channels != b->playback.cfg.pulse.channels) return false;
      if (strcmp(a->playback.cfg.pulse.device, b->playback.cfg.pulse.device) != 0) return false;
      break;
#endif
#if defined(ENABLE_PIPEWIRE)
    case AUDIO_BACKEND_TYPE_PIPEWIRE:
      if (a->playback.cfg.pipewire.channels != b->playback.cfg.pipewire.channels) return false;
      if (strcmp(a->playback.cfg.pipewire.device, b->playback.cfg.pipewire.device) != 0) return false;
      if (strcmp(a->playback.cfg.pipewire.node_name, b->playback.cfg.pipewire.node_name) != 0) return false;
      if (strcmp(a->playback.cfg.pipewire.node_description, b->playback.cfg.pipewire.node_description) != 0) return false;
      if (strcmp(a->playback.cfg.pipewire.node_group_name, b->playback.cfg.pipewire.node_group_name) != 0) return false;
      if (strcmp(a->playback.cfg.pipewire.autoconnect_to, b->playback.cfg.pipewire.autoconnect_to) != 0) return false;
      break;
#endif
#if defined(ENABLE_JACK)
    case AUDIO_BACKEND_TYPE_JACK:
      if (a->playback.cfg.jack.channels != b->playback.cfg.jack.channels) return false;
      if (strcmp(a->playback.cfg.jack.device, b->playback.cfg.jack.device) != 0) return false;
      break;
#endif
    case AUDIO_BACKEND_TYPE_FILE:
      if (a->playback.cfg.raw_file.channels != b->playback.cfg.raw_file.channels) return false;
      if (strcmp(a->playback.cfg.raw_file.filename, b->playback.cfg.raw_file.filename) != 0) return false;
      if (a->playback.cfg.raw_file.format != b->playback.cfg.raw_file.format) return false;
      if (a->playback.cfg.raw_file.wav_header != b->playback.cfg.raw_file.wav_header) return false;
      break;
    case AUDIO_BACKEND_TYPE_STDIN_OUT:
      if (a->playback.cfg.stdout_out.channels != b->playback.cfg.stdout_out.channels) return false;
      if (a->playback.cfg.stdout_out.format != b->playback.cfg.stdout_out.format) return false;
      if (a->playback.cfg.stdout_out.wav_header != b->playback.cfg.stdout_out.wav_header) return false;
      break;
#if defined(ENABLE_WASAPI)
    case AUDIO_BACKEND_TYPE_WASAPI:
      if (a->playback.cfg.wasapi.channels != b->playback.cfg.wasapi.channels) return false;
      if (strcmp(a->playback.cfg.wasapi.device, b->playback.cfg.wasapi.device) != 0) return false;
      if (a->playback.cfg.wasapi.format != b->playback.cfg.wasapi.format) return false;
      if (a->playback.cfg.wasapi.exclusive != b->playback.cfg.wasapi.exclusive) return false;
      if (a->playback.cfg.wasapi.polling != b->playback.cfg.wasapi.polling) return false;
      break;
#endif
#if defined(ENABLE_ASIO)
    case AUDIO_BACKEND_TYPE_ASIO:
      if (a->playback.cfg.asio.channels != b->playback.cfg.asio.channels) return false;
      if (strcmp(a->playback.cfg.asio.device, b->playback.cfg.asio.device) != 0) return false;
      if (a->playback.cfg.asio.format != b->playback.cfg.asio.format) return false;
      break;
#endif
    default:
      break;
  }

  return true;
}

/**
 * @brief Compares two pipeline step configurations for equality.
 * @param a Pointer to first pipeline step configuration.
 * @param b Pointer to second pipeline step configuration.
 * @return true if steps are equal, false otherwise.
 */
static bool pipeline_step_equal(const pipeline_step_t* a,
                                const pipeline_step_t* b) {
  if (a->type != b->type) return false;
  if (a->channel != b->channel) return false;
  if (a->has_channel != b->has_channel) return false;
  if (a->channels_count != b->channels_count) return false;
  for (size_t i = 0; i < a->channels_count; i++) {
    if (a->channels[i] != b->channels[i]) return false;
  }
  if (strcmp(a->name, b->name) != 0) return false;
  if (a->has_name != b->has_name) return false;
  if (!string_arrays_equal(a->names, a->names_count, b->names, b->names_count))
    return false;
  if (a->bypassed != b->bypassed) return false;
  return true;
}

config_change_type_t config_diff(const dsp_config_t* current,
                                 const dsp_config_t* new_conf,
                                 config_change_t* out_change) {
  if (!out_change) return CONFIG_CHANGE_DEVICES;
  if (!current || !new_conf) {
    out_change->type = CONFIG_CHANGE_DEVICES;
    return CONFIG_CHANGE_DEVICES;
  }

  // If devices config (sample rate, chunk size, backend config) changes,
  // we must rebuild the entire audio backend and pipeline.
  if (!devices_config_equal(&current->devices, &new_conf->devices)) {
    out_change->type = CONFIG_CHANGE_DEVICES;
    return CONFIG_CHANGE_DEVICES;
  }

  // If pipeline steps count or details change, we must rebuild the pipeline.
  if (current->pipeline_count != new_conf->pipeline_count) {
    out_change->type = CONFIG_CHANGE_PIPELINE;
    return CONFIG_CHANGE_PIPELINE;
  }
  for (size_t i = 0; i < current->pipeline_count; i++) {
    if (!pipeline_step_equal(&current->pipeline[i], &new_conf->pipeline[i])) {
      out_change->type = CONFIG_CHANGE_PIPELINE;
      return CONFIG_CHANGE_PIPELINE;
    }
  }

  // If the count of filters, mixers, or processors changes, it indicates
  // structural change.
  if (current->filters_count != new_conf->filters_count ||
      current->mixers_count != new_conf->mixers_count ||
      current->processors_count != new_conf->processors_count) {
    out_change->type = CONFIG_CHANGE_PIPELINE;
    return CONFIG_CHANGE_PIPELINE;
  }

  // If names of filters/mixers/processors at specific slots change, it's also a
  // structural change.
  for (size_t i = 0; i < current->filters_count; i++) {
    if (strcmp(current->filters[i].name, new_conf->filters[i].name) != 0) {
      out_change->type = CONFIG_CHANGE_PIPELINE;
      return CONFIG_CHANGE_PIPELINE;
    }
  }
  for (size_t i = 0; i < current->mixers_count; i++) {
    if (strcmp(current->mixers[i].name, new_conf->mixers[i].name) != 0) {
      out_change->type = CONFIG_CHANGE_PIPELINE;
      return CONFIG_CHANGE_PIPELINE;
    }
  }
  for (size_t i = 0; i < current->processors_count; i++) {
    if (strcmp(current->processors[i].name, new_conf->processors[i].name) !=
        0) {
      out_change->type = CONFIG_CHANGE_PIPELINE;
      return CONFIG_CHANGE_PIPELINE;
    }
  }

  // If we reach here, the structure of the pipeline is identical.
  // We check which individual components had their parameters modified.
  char** changed_filters = NULL;
  char** changed_mixers = NULL;
  char** changed_processors = NULL;
  size_t cf_count = 0;
  size_t cm_count = 0;
  size_t cp_count = 0;

  if (current->filters_count > 0) {
    changed_filters = malloc(sizeof(char*) * current->filters_count);
    if (!changed_filters) goto error_cleanup;
  }
  for (size_t i = 0; i < current->filters_count; i++) {
    if (!filter_config_equal(&current->filters[i].filter,
                             &new_conf->filters[i].filter)) {
      char* name_copy = strdup(current->filters[i].name);
      if (!name_copy) goto error_cleanup;
      changed_filters[cf_count++] = name_copy;
    }
  }

  if (current->mixers_count > 0) {
    changed_mixers = malloc(sizeof(char*) * current->mixers_count);
    if (!changed_mixers) goto error_cleanup;
  }
  for (size_t i = 0; i < current->mixers_count; i++) {
    if (!mixer_config_equal(&current->mixers[i].mixer,
                            &new_conf->mixers[i].mixer)) {
      char* name_copy = strdup(current->mixers[i].name);
      if (!name_copy) goto error_cleanup;
      changed_mixers[cm_count++] = name_copy;
    }
  }

  if (current->processors_count > 0) {
    changed_processors = malloc(sizeof(char*) * current->processors_count);
    if (!changed_processors) goto error_cleanup;
  }
  for (size_t i = 0; i < current->processors_count; i++) {
    if (!processor_config_equal(&current->processors[i].processor,
                                &new_conf->processors[i].processor)) {
      char* name_copy = strdup(current->processors[i].name);
      if (!name_copy) goto error_cleanup;
      changed_processors[cp_count++] = name_copy;
    }
  }

  // If no parameters changed, we have no changes at all.
  if (cf_count == 0 && cm_count == 0 && cp_count == 0) {
    if (changed_filters) free(changed_filters);
    if (changed_mixers) free(changed_mixers);
    if (changed_processors) free(changed_processors);
    out_change->type = CONFIG_CHANGE_NONE;
    return CONFIG_CHANGE_NONE;
  }

  out_change->filters = changed_filters;
  out_change->filters_count = cf_count;
  out_change->mixers = changed_mixers;
  out_change->mixers_count = cm_count;
  out_change->processors = changed_processors;
  out_change->processors_count = cp_count;

  // Mixer parameter changes require special handling (sometimes thread safety
  // logic), so we separate them from normal filter parameter updates.
  if (cm_count > 0) {
    out_change->type = CONFIG_CHANGE_MIXER_PARAMETERS;
  } else {
    out_change->type = CONFIG_CHANGE_FILTER_PARAMETERS;
  }

  return out_change->type;

error_cleanup:
  if (changed_filters) {
    for (size_t i = 0; i < cf_count; i++) free(changed_filters[i]);
    free(changed_filters);
  }
  if (changed_mixers) {
    for (size_t i = 0; i < cm_count; i++) free(changed_mixers[i]);
    free(changed_mixers);
  }
  if (changed_processors) {
    for (size_t i = 0; i < cp_count; i++) free(changed_processors[i]);
    free(changed_processors);
  }
  out_change->type = CONFIG_CHANGE_PIPELINE;
  return CONFIG_CHANGE_PIPELINE;
}

config_change_t* config_change_create(void) {
  config_change_t* change = malloc(sizeof(struct config_change));
  if (change) {
    memset(change, 0, sizeof(struct config_change));
  }
  return change;
}

void config_change_free(config_change_t* change) {
  if (!change) return;
  if (change->filters) {
    for (size_t i = 0; i < change->filters_count; i++) free(change->filters[i]);
    free(change->filters);
  }
  if (change->mixers) {
    for (size_t i = 0; i < change->mixers_count; i++) free(change->mixers[i]);
    free(change->mixers);
  }
  if (change->processors) {
    for (size_t i = 0; i < change->processors_count; i++)
      free(change->processors[i]);
    free(change->processors);
  }
  free(change);
}

char** config_change_take_filters(config_change_t* change, size_t* out_count) {
  if (!change || !out_count) return NULL;
  char** res = change->filters;
  *out_count = change->filters_count;
  change->filters = NULL;
  change->filters_count = 0;
  return res;
}

char** config_change_take_mixers(config_change_t* change, size_t* out_count) {
  if (!change || !out_count) return NULL;
  char** res = change->mixers;
  *out_count = change->mixers_count;
  change->mixers = NULL;
  change->mixers_count = 0;
  return res;
}

char** config_change_take_processors(config_change_t* change,
                                     size_t* out_count) {
  if (!change || !out_count) return NULL;
  char** res = change->processors;
  *out_count = change->processors_count;
  change->processors = NULL;
  change->processors_count = 0;
  return res;
}
