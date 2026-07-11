// Resampler protocol + shared types.
// Four resampler implementations conform to AudioResampler:
//   * SynchronousResampler — FFT-based fixed-ratio.
//   * AsyncSincResampler   — Asynchronous windowed-sinc resampler.
//   * AsyncPolyResampler   — Asynchronous polynomial resampler.
//   * AppleResampler       — Core Audio AudioConverter wrapper.

#include "audio_resampler.h"

struct audio_resampler {
  resampler_impl_type_t type;
  void* impl;
  resampler_error_t (*process)(struct audio_resampler* self,
                               const audio_chunk_t* input,
                               audio_chunk_t* output);
  void (*set_relative_ratio)(struct audio_resampler* self, double multiplier);
  double (*get_ratio)(const struct audio_resampler* self);
  size_t (*get_max_output_frames)(const struct audio_resampler* self);
  size_t (*get_chunk_size)(const struct audio_resampler* self);
  size_t (*get_channels)(const struct audio_resampler* self);
  void (*free)(struct audio_resampler* self);
};

#include <stdlib.h>
#include <string.h>

sinc_interpolation_type_t sinc_interpolation_type_from_string(const char* str) {
  if (!str) return SINC_INTERPOLATION_QUADRATIC;
  if (strcmp(str, "Nearest") == 0) return SINC_INTERPOLATION_NEAREST;
  if (strcmp(str, "Linear") == 0) return SINC_INTERPOLATION_LINEAR;
  if (strcmp(str, "Quadratic") == 0) return SINC_INTERPOLATION_QUADRATIC;
  if (strcmp(str, "Cubic") == 0) return SINC_INTERPOLATION_CUBIC;
  return SINC_INTERPOLATION_QUADRATIC;
}

poly_interpolation_t poly_interpolation_from_string(const char* str) {
  if (!str) return POLY_INTERPOLATION_CUBIC;
  if (strcmp(str, "Linear") == 0) return POLY_INTERPOLATION_LINEAR;
  if (strcmp(str, "Cubic") == 0) return POLY_INTERPOLATION_CUBIC;
  if (strcmp(str, "Quintic") == 0) return POLY_INTERPOLATION_QUINTIC;
  if (strcmp(str, "Septic") == 0) return POLY_INTERPOLATION_SEPTIC;
  return POLY_INTERPOLATION_CUBIC;
}

/**
 * @brief process wrapper for synchronous resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @param input Pointer to the input audio chunk.
 * @param output Pointer to the output audio chunk.
 * @return resampler_error_t Status code.
 */
static resampler_error_t sync_process(audio_resampler_t* self,
                                      const audio_chunk_t* input,
                                      audio_chunk_t* output) {
  return synchronous_resampler_process((synchronous_resampler_t*)self->impl,
                                       input, output);
}

/**
 * @brief set_relative_ratio wrapper for synchronous resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @param mult The ratio multiplier.
 */
static void sync_set_ratio(audio_resampler_t* self, double mult) {
  synchronous_resampler_set_relative_ratio((synchronous_resampler_t*)self->impl,
                                           mult);
}

/**
 * @brief get_ratio wrapper for synchronous resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @return The current resample ratio.
 */
static double sync_get_ratio(const audio_resampler_t* self) {
  return synchronous_resampler_get_ratio(
      (const synchronous_resampler_t*)self->impl);
}

/**
 * @brief get_max_output_frames wrapper for synchronous resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @return The maximum number of output frames.
 */
static size_t sync_get_max(const audio_resampler_t* self) {
  return synchronous_resampler_get_max_output_frames(
      (const synchronous_resampler_t*)self->impl);
}

/**
 * @brief get_chunk_size wrapper for synchronous resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @return The chunk size.
 */
static size_t sync_get_cs(const audio_resampler_t* self) {
  return synchronous_resampler_get_chunk_size(
      (const synchronous_resampler_t*)self->impl);
}

/**
 * @brief get_channels wrapper for synchronous resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @return The number of channels.
 */
static size_t sync_get_ch(const audio_resampler_t* self) {
  return synchronous_resampler_get_channels(
      (const synchronous_resampler_t*)self->impl);
}

/**
 * @brief free wrapper for synchronous resampler.
 * @param self Pointer to the audio_resampler wrapper to free.
 */
static void sync_free(audio_resampler_t* self) {
  if (self->impl)
    synchronous_resampler_free((synchronous_resampler_t*)self->impl);
  free(self);
}

audio_resampler_t* audio_resampler_wrap_synchronous(
    synchronous_resampler_t* res) {
  if (!res) return NULL;
  audio_resampler_t* wrap =
      (audio_resampler_t*)calloc(1, sizeof(audio_resampler_t));
  if (!wrap) {
    synchronous_resampler_free(res);
    return NULL;
  }
  wrap->type = RESAMPLER_IMPL_SYNCHRONOUS;
  wrap->impl = res;
  wrap->process = sync_process;
  wrap->set_relative_ratio = sync_set_ratio;
  wrap->get_ratio = sync_get_ratio;
  wrap->get_max_output_frames = sync_get_max;
  wrap->get_chunk_size = sync_get_cs;
  wrap->get_channels = sync_get_ch;
  wrap->free = sync_free;
  return wrap;
}

/**
 * @brief process wrapper for async sinc resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @param input Pointer to the input audio chunk.
 * @param output Pointer to the output audio chunk.
 * @return resampler_error_t Status code.
 */
static resampler_error_t sinc_process(audio_resampler_t* self,
                                      const audio_chunk_t* input,
                                      audio_chunk_t* output) {
  return async_sinc_resampler_process((async_sinc_resampler_t*)self->impl,
                                      input, output);
}

/**
 * @brief set_relative_ratio wrapper for async sinc resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @param mult The ratio multiplier.
 */
static void sinc_set_ratio(audio_resampler_t* self, double mult) {
  async_sinc_resampler_set_relative_ratio((async_sinc_resampler_t*)self->impl,
                                          mult);
}

/**
 * @brief get_ratio wrapper for async sinc resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @return The current resample ratio.
 */
static double sinc_get_ratio(const audio_resampler_t* self) {
  return async_sinc_resampler_get_ratio(
      (const async_sinc_resampler_t*)self->impl);
}

/**
 * @brief get_max_output_frames wrapper for async sinc resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @return The maximum number of output frames.
 */
static size_t sinc_get_max(const audio_resampler_t* self) {
  return async_sinc_resampler_get_max_output_frames(
      (const async_sinc_resampler_t*)self->impl);
}

/**
 * @brief get_chunk_size wrapper for async sinc resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @return The chunk size.
 */
static size_t sinc_get_cs(const audio_resampler_t* self) {
  return async_sinc_resampler_get_chunk_size(
      (const async_sinc_resampler_t*)self->impl);
}

/**
 * @brief get_channels wrapper for async sinc resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @return The number of channels.
 */
static size_t sinc_get_ch(const audio_resampler_t* self) {
  return async_sinc_resampler_get_channels(
      (const async_sinc_resampler_t*)self->impl);
}

/**
 * @brief free wrapper for async sinc resampler.
 * @param self Pointer to the audio_resampler wrapper to free.
 */
static void sinc_free(audio_resampler_t* self) {
  if (self->impl)
    async_sinc_resampler_free((async_sinc_resampler_t*)self->impl);
  free(self);
}

audio_resampler_t* audio_resampler_wrap_async_sinc(
    async_sinc_resampler_t* res) {
  if (!res) return NULL;
  audio_resampler_t* wrap =
      (audio_resampler_t*)calloc(1, sizeof(audio_resampler_t));
  if (!wrap) {
    async_sinc_resampler_free(res);
    return NULL;
  }
  wrap->type = RESAMPLER_IMPL_ASYNC_SINC;
  wrap->impl = res;
  wrap->process = sinc_process;
  wrap->set_relative_ratio = sinc_set_ratio;
  wrap->get_ratio = sinc_get_ratio;
  wrap->get_max_output_frames = sinc_get_max;
  wrap->get_chunk_size = sinc_get_cs;
  wrap->get_channels = sinc_get_ch;
  wrap->free = sinc_free;
  return wrap;
}

/**
 * @brief process wrapper for async poly resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @param input Pointer to the input audio chunk.
 * @param output Pointer to the output audio chunk.
 * @return resampler_error_t Status code.
 */
static resampler_error_t poly_process(audio_resampler_t* self,
                                      const audio_chunk_t* input,
                                      audio_chunk_t* output) {
  return async_poly_resampler_process((async_poly_resampler_t*)self->impl,
                                      input, output);
}

/**
 * @brief set_relative_ratio wrapper for async poly resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @param mult The ratio multiplier.
 */
static void poly_set_ratio(audio_resampler_t* self, double mult) {
  async_poly_resampler_set_relative_ratio((async_poly_resampler_t*)self->impl,
                                          mult);
}

/**
 * @brief get_ratio wrapper for async poly resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @return The current resample ratio.
 */
static double poly_get_ratio(const audio_resampler_t* self) {
  return async_poly_resampler_get_ratio(
      (const async_poly_resampler_t*)self->impl);
}

/**
 * @brief get_max_output_frames wrapper for async poly resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @return The maximum number of output frames.
 */
static size_t poly_get_max(const audio_resampler_t* self) {
  return async_poly_resampler_get_max_output_frames(
      (const async_poly_resampler_t*)self->impl);
}

/**
 * @brief get_chunk_size wrapper for async poly resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @return The chunk size.
 */
static size_t poly_get_cs(const audio_resampler_t* self) {
  return async_poly_resampler_get_chunk_size(
      (const async_poly_resampler_t*)self->impl);
}

/**
 * @brief get_channels wrapper for async poly resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @return The number of channels.
 */
static size_t poly_get_ch(const audio_resampler_t* self) {
  return async_poly_resampler_get_channels(
      (const async_poly_resampler_t*)self->impl);
}

/**
 * @brief free wrapper for async poly resampler.
 * @param self Pointer to the audio_resampler wrapper to free.
 */
static void poly_free(audio_resampler_t* self) {
  if (self->impl)
    async_poly_resampler_free((async_poly_resampler_t*)self->impl);
  free(self);
}

audio_resampler_t* audio_resampler_wrap_async_poly(
    async_poly_resampler_t* res) {
  if (!res) return NULL;
  audio_resampler_t* wrap =
      (audio_resampler_t*)calloc(1, sizeof(audio_resampler_t));
  if (!wrap) {
    async_poly_resampler_free(res);
    return NULL;
  }
  wrap->type = RESAMPLER_IMPL_ASYNC_POLY;
  wrap->impl = res;
  wrap->process = poly_process;
  wrap->set_relative_ratio = poly_set_ratio;
  wrap->get_ratio = poly_get_ratio;
  wrap->get_max_output_frames = poly_get_max;
  wrap->get_chunk_size = poly_get_cs;
  wrap->get_channels = poly_get_ch;
  wrap->free = poly_free;
  return wrap;
}

#ifdef ENABLE_COREAUDIO
/**
 * @brief process wrapper for Apple resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @param input Pointer to the input audio chunk.
 * @param output Pointer to the output audio chunk.
 * @return resampler_error_t Status code.
 */
static resampler_error_t apple_process(audio_resampler_t* self,
                                       const audio_chunk_t* input,
                                       audio_chunk_t* output) {
  return apple_resampler_process((apple_resampler_t*)self->impl, input, output);
}

/**
 * @brief set_relative_ratio wrapper for Apple resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @param mult The ratio multiplier.
 */
static void apple_set_ratio(audio_resampler_t* self, double mult) {
  apple_resampler_set_relative_ratio((apple_resampler_t*)self->impl, mult);
}

/**
 * @brief get_ratio wrapper for Apple resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @return The current resample ratio.
 */
static double apple_get_ratio(const audio_resampler_t* self) {
  return apple_resampler_get_ratio((const apple_resampler_t*)self->impl);
}

/**
 * @brief get_max_output_frames wrapper for Apple resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @return The maximum number of output frames.
 */
static size_t apple_get_max(const audio_resampler_t* self) {
  return apple_resampler_get_max_output_frames(
      (const apple_resampler_t*)self->impl);
}

/**
 * @brief get_chunk_size wrapper for Apple resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @return The chunk size.
 */
static size_t apple_get_cs(const audio_resampler_t* self) {
  return apple_resampler_get_chunk_size((const apple_resampler_t*)self->impl);
}

/**
 * @brief get_channels wrapper for Apple resampler.
 * @param self Pointer to the audio_resampler wrapper.
 * @return The number of channels.
 */
static size_t apple_get_ch(const audio_resampler_t* self) {
  return apple_resampler_get_channels((const apple_resampler_t*)self->impl);
}

/**
 * @brief free wrapper for Apple resampler.
 * @param self Pointer to the audio_resampler wrapper to free.
 */
static void apple_free_fn(audio_resampler_t* self) {
  if (self->impl) apple_resampler_free((apple_resampler_t*)self->impl);
  free(self);
}

audio_resampler_t* audio_resampler_wrap_apple(apple_resampler_t* res) {
  if (!res) return NULL;
  audio_resampler_t* wrap =
      (audio_resampler_t*)calloc(1, sizeof(audio_resampler_t));
  if (!wrap) {
    apple_resampler_free(res);
    return NULL;
  }
  wrap->type = RESAMPLER_IMPL_APPLE;
  wrap->impl = res;
  wrap->process = apple_process;
  wrap->set_relative_ratio = apple_set_ratio;
  wrap->get_ratio = apple_get_ratio;
  wrap->get_max_output_frames = apple_get_max;
  wrap->get_chunk_size = apple_get_cs;
  wrap->get_channels = apple_get_ch;
  wrap->free = apple_free_fn;
  return wrap;
}
#endif

audio_resampler_t* audio_resampler_create_from_config(
    const resampler_config_t* config, size_t input_rate, size_t output_rate,
    size_t channels, size_t chunk_size, config_error_t* err) {
  if (!config) {
    config_error_set(err, CONFIG_ERR_PARSE, "Resampler config is NULL");
    return NULL;
  }
  switch (config->type) {
    case RESAMPLER_TYPE_SYNCHRONOUS: {
      synchronous_resampler_t* res = synchronous_resampler_create(
          channels, input_rate, output_rate, chunk_size, err);
      if (!res) return NULL;
      audio_resampler_t* wrap = audio_resampler_wrap_synchronous(res);
      if (!wrap) {
        config_error_set(err, CONFIG_ERR_PARSE, "Failed to wrap synchronous resampler");
      }
      return wrap;
    }
    case RESAMPLER_TYPE_ASYNC_SINC: {
      if (config->has_sinc_len && config->has_oversampling_factor &&
          config->has_window && config->has_interpolation) {
        window_function_t wf = window_function_from_string(
            config->window, WINDOW_FUNCTION_BLACKMAN_HARRIS2);
        sinc_interpolation_type_t interp =
            sinc_interpolation_type_from_string(config->interpolation);
        async_sinc_resampler_t* res = async_sinc_resampler_create(
            channels, input_rate, output_rate, (size_t)config->sinc_len,
            (size_t)config->oversampling_factor, interp, wf, config->f_cutoff,
            config->has_f_cutoff, chunk_size, 1.1, err);
        if (!res) return NULL;
        audio_resampler_t* wrap = audio_resampler_wrap_async_sinc(res);
        if (!wrap) {
          config_error_set(err, CONFIG_ERR_PARSE, "Failed to wrap async sinc resampler");
        }
        return wrap;
      } else {
        resampler_profile_t prof = RESAMPLER_PROFILE_BALANCED;
        if (config->has_profile) {
          prof = resampler_profile_from_string(config->profile);
        }
        async_sinc_resampler_t* res = async_sinc_resampler_create_from_profile(
            channels, input_rate, output_rate, prof, chunk_size, 1.1, err);
        if (!res) return NULL;
        audio_resampler_t* wrap = audio_resampler_wrap_async_sinc(res);
        if (!wrap) {
          config_error_set(err, CONFIG_ERR_PARSE, "Failed to wrap async sinc resampler");
        }
        return wrap;
      }
    }
    case RESAMPLER_TYPE_ASYNC_POLY: {
      poly_interpolation_t interp = POLY_INTERPOLATION_CUBIC;
      if (config->has_interpolation) {
        interp = poly_interpolation_from_string(config->interpolation);
      }
      async_poly_resampler_t* res = async_poly_resampler_create(
          channels, input_rate, output_rate, interp, chunk_size, 1.1, err);
      if (!res) return NULL;
      audio_resampler_t* wrap = audio_resampler_wrap_async_poly(res);
      if (!wrap) {
        config_error_set(err, CONFIG_ERR_PARSE, "Failed to wrap async poly resampler");
      }
      return wrap;
    }
#if defined(ENABLE_COREAUDIO)
    case RESAMPLER_TYPE_APPLE: {
      apple_resampler_quality_t qual = config->has_apple_quality
                                           ? config->apple_quality
                                           : APPLE_RESAMPLER_QUALITY_MAX;
      apple_resampler_complexity_t comp =
          config->has_apple_complexity ? config->apple_complexity
                                       : APPLE_RESAMPLER_COMPLEXITY_NORMAL;
      apple_resampler_t* res = apple_resampler_create(
          channels, input_rate, output_rate, qual, comp, chunk_size, err);
      if (!res) return NULL;
      audio_resampler_t* wrap = audio_resampler_wrap_apple(res);
      if (!wrap) {
        config_error_set(err, CONFIG_ERR_PARSE, "Failed to wrap Apple resampler");
      }
      return wrap;
    }
#endif
    default:
      config_error_set(err, CONFIG_ERR_PARSE, "Unknown resampler type %d", config->type);
      return NULL;
  }
}

resampler_error_t audio_resampler_process(audio_resampler_t* resampler,
                                          const audio_chunk_t* input,
                                          audio_chunk_t* output) {
  if (!resampler || !resampler->process) return RESAMPLER_ERR_INVALID_PARAMETER;
  return resampler->process(resampler, input, output);
}

void audio_resampler_set_relative_ratio(audio_resampler_t* resampler,
                                        double multiplier) {
  if (resampler && resampler->set_relative_ratio) {
    resampler->set_relative_ratio(resampler, multiplier);
  }
}

double audio_resampler_get_ratio(const audio_resampler_t* resampler) {
  return (resampler && resampler->get_ratio) ? resampler->get_ratio(resampler)
                                             : 1.0;
}

size_t audio_resampler_get_max_output_frames(
    const audio_resampler_t* resampler) {
  return (resampler && resampler->get_max_output_frames)
             ? resampler->get_max_output_frames(resampler)
             : 0;
}

size_t audio_resampler_get_chunk_size(const audio_resampler_t* resampler) {
  return (resampler && resampler->get_chunk_size)
             ? resampler->get_chunk_size(resampler)
             : 0;
}

size_t audio_resampler_get_channels(const audio_resampler_t* resampler) {
  return (resampler && resampler->get_channels)
             ? resampler->get_channels(resampler)
             : 0;
}

void audio_resampler_free(audio_resampler_t* resampler) {
  if (resampler && resampler->free) {
    resampler->free(resampler);
  }
}
