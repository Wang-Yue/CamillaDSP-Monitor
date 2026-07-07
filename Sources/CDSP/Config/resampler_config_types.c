#include "resampler_config_types.h"

#include <stdio.h>
#include <string.h>

// Standalone resampler configuration types.

const char* resampler_type_to_string(resampler_type_t type) {
  switch (type) {
    case RESAMPLER_TYPE_SYNCHRONOUS:
      return "Synchronous";
#if defined(ENABLE_COREAUDIO)
    case RESAMPLER_TYPE_APPLE:
      return "Apple";
#endif
    case RESAMPLER_TYPE_ASYNC_SINC:
      return "AsyncSinc";
    case RESAMPLER_TYPE_ASYNC_POLY:
      return "AsyncPoly";
    default:
      return "Synchronous";
  }
}

resampler_type_t resampler_type_from_string(const char* str) {
  if (!str) return RESAMPLER_TYPE_SYNCHRONOUS;
  if (strcmp(str, "Synchronous") == 0) return RESAMPLER_TYPE_SYNCHRONOUS;
#if defined(ENABLE_COREAUDIO)
  if (strcmp(str, "Apple") == 0) return RESAMPLER_TYPE_APPLE;
#endif
  if (strcmp(str, "AsyncSinc") == 0) return RESAMPLER_TYPE_ASYNC_SINC;
  if (strcmp(str, "AsyncPoly") == 0) return RESAMPLER_TYPE_ASYNC_POLY;
  return RESAMPLER_TYPE_SYNCHRONOUS;
}

#if defined(ENABLE_COREAUDIO)
/// Quality settings supported by Apple's AudioConverter.
const char* apple_resampler_quality_to_string(
    apple_resampler_quality_t quality) {
  switch (quality) {
    case APPLE_RESAMPLER_QUALITY_MIN:
      return "Min";
    case APPLE_RESAMPLER_QUALITY_LOW:
      return "Low";
    case APPLE_RESAMPLER_QUALITY_MEDIUM:
      return "Medium";
    case APPLE_RESAMPLER_QUALITY_HIGH:
      return "High";
    case APPLE_RESAMPLER_QUALITY_MAX:
      return "Max";
    default:
      return "Medium";
  }
}

apple_resampler_quality_t apple_resampler_quality_from_string(const char* str) {
  if (!str) return APPLE_RESAMPLER_QUALITY_MEDIUM;
  if (strcmp(str, "Min") == 0) return APPLE_RESAMPLER_QUALITY_MIN;
  if (strcmp(str, "Low") == 0) return APPLE_RESAMPLER_QUALITY_LOW;
  if (strcmp(str, "Medium") == 0) return APPLE_RESAMPLER_QUALITY_MEDIUM;
  if (strcmp(str, "High") == 0) return APPLE_RESAMPLER_QUALITY_HIGH;
  if (strcmp(str, "Max") == 0) return APPLE_RESAMPLER_QUALITY_MAX;
  return APPLE_RESAMPLER_QUALITY_MEDIUM;
}

/// Algorithm complexity supported by Apple's AudioConverter.
const char* apple_resampler_complexity_to_string(
    apple_resampler_complexity_t comp) {
  switch (comp) {
    case APPLE_RESAMPLER_COMPLEXITY_LINEAR:
      return "Linear";
    case APPLE_RESAMPLER_COMPLEXITY_NORMAL:
      return "Normal";
    case APPLE_RESAMPLER_COMPLEXITY_MASTERING:
      return "Mastering";
    case APPLE_RESAMPLER_COMPLEXITY_MINIMUM_PHASE:
      return "MinimumPhase";
    default:
      return "Normal";
  }
}

apple_resampler_complexity_t apple_resampler_complexity_from_string(
    const char* str) {
  if (!str) return APPLE_RESAMPLER_COMPLEXITY_NORMAL;
  if (strcmp(str, "Linear") == 0) return APPLE_RESAMPLER_COMPLEXITY_LINEAR;
  if (strcmp(str, "Normal") == 0) return APPLE_RESAMPLER_COMPLEXITY_NORMAL;
  if (strcmp(str, "Mastering") == 0)
    return APPLE_RESAMPLER_COMPLEXITY_MASTERING;
  if (strcmp(str, "MinimumPhase") == 0)
    return APPLE_RESAMPLER_COMPLEXITY_MINIMUM_PHASE;
  return APPLE_RESAMPLER_COMPLEXITY_NORMAL;
}

uint32_t apple_resampler_complexity_os_type(apple_resampler_complexity_t comp) {
  switch (comp) {
    case APPLE_RESAMPLER_COMPLEXITY_LINEAR:
      return 0x6C696E65;  // 'line'
    case APPLE_RESAMPLER_COMPLEXITY_NORMAL:
      return 0x6E6F726D;  // 'norm'
    case APPLE_RESAMPLER_COMPLEXITY_MASTERING:
      return 0x62617473;  // 'bats'
    case APPLE_RESAMPLER_COMPLEXITY_MINIMUM_PHASE:
      return 0x6D696E70;  // 'minp'
    default:
      return 0x6E6F726D;
  }
}
#endif  // ENABLE_COREAUDIO

const char* resampler_profile_to_string(resampler_profile_t profile) {
  switch (profile) {
    case RESAMPLER_PROFILE_VERY_FAST:
      return "VeryFast";
    case RESAMPLER_PROFILE_FAST:
      return "Fast";
    case RESAMPLER_PROFILE_BALANCED:
      return "Balanced";
    case RESAMPLER_PROFILE_ACCURATE:
      return "Accurate";
    default:
      return "Balanced";
  }
}

resampler_profile_t resampler_profile_from_string(const char* str) {
  if (!str) return RESAMPLER_PROFILE_BALANCED;
  if (strcmp(str, "VeryFast") == 0) return RESAMPLER_PROFILE_VERY_FAST;
  if (strcmp(str, "Fast") == 0) return RESAMPLER_PROFILE_FAST;
  if (strcmp(str, "Balanced") == 0) return RESAMPLER_PROFILE_BALANCED;
  if (strcmp(str, "Accurate") == 0) return RESAMPLER_PROFILE_ACCURATE;
  return RESAMPLER_PROFILE_BALANCED;
}

void resampler_config_init(resampler_config_t* config, resampler_type_t type) {
  if (!config) return;
  memset(config, 0, sizeof(resampler_config_t));
  config->type = type;
}

void resampler_config_description(const resampler_config_t* config,
                                  char* out_buf, size_t buf_len) {
  if (!config || !out_buf || buf_len == 0) return;
  const char* prof = config->has_profile ? config->profile : "nil";
  const char* interp =
      config->has_interpolation ? config->interpolation : "nil";
  int sinc = config->has_sinc_len ? config->sinc_len : 0;
  snprintf(
      out_buf, buf_len,
      "ResamplerConfig(type: %s, profile: %s, interpolation: %s, sincLen: %d)",
      resampler_type_to_string(config->type), prof, interp, sinc);
}
