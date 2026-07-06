/**
 * @file mixer.c
 * @brief Implementation of the channel routing matrix and audio mixer.
 *
 * Channel Routing Matrix Implementation Details:
 * - The mixer converts user-configured mapping rules into precomputed
 * `prepared_source_list_t` structures per destination channel.
 * - Linear gain conversion: When gain scale is `GAIN_SCALE_DB`, dB values are
 * converted to linear gain using `double_from_db`.
 * - Phase inversion: If `inverted` is set to true, the linear gain is negated
 * (-lin_gain).
 * - Real-time processing (`audio_mixer_process`):
 *   1. Validates that input frames do not exceed `chunk_size` and destination
 * buffer matches `channels_out`.
 *   2. For each destination channel, clears the output waveform buffer to 0.0.
 *   3. Iterates over contributing prepared sources:
 *      - If gain == 1.0, uses `dsp_ops_add` (vectorized addition).
 *      - If gain != 0.0 and != 1.0, uses `dsp_ops_multiply_add` (vectorized
 * multiply-accumulate).
 *   4. Zero-allocation guarantee is strictly maintained on the audio processing
 * path.
 */
#if defined(__linux__)
#define _GNU_SOURCE
#endif
#include "Mixer/mixer.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief Populates the internal routing matrix mapping from a mixer
 * configuration.
 *
 * Precomputes linear gains and channel routing lists for all destination
 * channels.
 *
 * @param mixer Pointer to audio mixer instance.
 * @param config Configuration containing mapping rules.
 */
static void populate_mapping(audio_mixer_t* mixer,
                             const mixer_config_t* config) {
  for (size_t i = 0; i < config->mapping_count; i++) {
    const mixer_mapping_t* map = &config->mapping[i];
    size_t dest = (size_t)map->dest;
    // Ignore mappings to out-of-bounds destination channels or muted
    // destination mappings
    if (dest >= mixer->channels_out || map->mute) continue;

    // Count unmuted contributing sources for this destination channel
    size_t valid_count = 0;
    for (size_t j = 0; j < map->sources_count; j++) {
      if (!map->sources[j].mute) valid_count++;
    }
    if (valid_count == 0) continue;

    // Allocate prepared source array for this destination channel
    mixer->mapping[dest].sources =
        (prepared_source_t*)malloc(valid_count * sizeof(prepared_source_t));
    if (!mixer->mapping[dest].sources) continue;
    mixer->mapping[dest].count = valid_count;

    size_t idx = 0;
    for (size_t j = 0; j < map->sources_count; j++) {
      const mixer_source_t* src = &map->sources[j];
      if (src->mute) continue;

      // Calculate linear gain from dB or linear configuration
      double gain = src->has_gain ? src->gain : 0.0;
      double lin_gain =
          (src->scale == GAIN_SCALE_LINEAR) ? gain : double_from_db(gain);
      // Invert phase if requested
      if (src->inverted) {
        lin_gain *= -1.0;
      }
      mixer->mapping[dest].sources[idx].in_channel = (size_t)src->channel;
      mixer->mapping[dest].sources[idx].gain = lin_gain;
      idx++;
    }
  }
}

audio_mixer_t* audio_mixer_create(const char* name,
                                  const mixer_config_t* config,
                                  size_t chunk_size) {
  if (!config) return NULL;
  audio_mixer_t* mixer = (audio_mixer_t*)calloc(1, sizeof(audio_mixer_t));
  if (!mixer) return NULL;

  mixer->chunk_size = chunk_size;
  mixer->name = name ? strdup(name) : strdup("mixer");
  mixer->channels_in = (size_t)config->channels_in;
  mixer->channels_out = (size_t)config->channels_out;
  mixer->mapping = (prepared_source_list_t*)calloc(
      mixer->channels_out, sizeof(prepared_source_list_t));
  if (!mixer->mapping) {
    free(mixer->name);
    free(mixer);
    return NULL;
  }

  populate_mapping(mixer, config);
  return mixer;
}

/// Zero-allocation API. The caller pre-allocates `output` with
/// `output.channels == channelsOut` and `output.frames >= input.validFrames`.
/// The mixer writes the mixed samples directly and sets `output.validFrames`.
///
/// `input` and `output` must reference distinct buffers — the mixer
/// accumulates into the output and reads input concurrently, so aliasing
/// would corrupt the result.
mixer_error_t audio_mixer_process(audio_mixer_t* mixer,
                                  const audio_chunk_t* input,
                                  audio_chunk_t* output) {
  if (!mixer || !input || !output) return MIXER_ERR_INPUT_SIZE_MISMATCH;
  size_t frames = input->valid_frames;
  if (frames > mixer->chunk_size) {
    return MIXER_ERR_INPUT_SIZE_MISMATCH;
  }
  if (audio_chunk_get_channels(output) != mixer->channels_out) {
    return MIXER_ERR_CHANNEL_COUNT_MISMATCH;
  }
  if (audio_chunk_get_frames(output) < frames) {
    return MIXER_ERR_OUTPUT_BUFFER_TOO_SMALL;
  }

  // Process each output destination channel
  for (size_t out_ch = 0; out_ch < mixer->channels_out; out_ch++) {
    mutable_waveform_t dst = audio_chunk_get_channel(output, out_ch);
    // Clear destination buffer before accumulating source contributions
    dsp_ops_clear(dst, frames);

    prepared_source_list_t* list = &mixer->mapping[out_ch];
    for (size_t i = 0; i < list->count; i++) {
      prepared_source_t* src = &list->sources[i];
      if (src->in_channel >= audio_chunk_get_channels(input)) continue;
      waveform_t src_ptr = audio_chunk_get_channel(input, src->in_channel);

      // Optimize direct unity gain addition vs multiply-accumulate
      if (src->gain == 1.0) {
        dsp_ops_add(src_ptr, dst, frames);
      } else if (src->gain != 0.0) {
        dsp_ops_multiply_add(src_ptr, src->gain, dst, frames);
      }
    }
  }
  output->valid_frames = frames;
  return MIXER_OK;
}

audio_chunk_t* audio_mixer_process_chunk(audio_mixer_t* mixer,
                                         const audio_chunk_t* input) {
  if (!mixer || !input) return NULL;
  audio_chunk_t* output =
      audio_chunk_create(input->valid_frames, mixer->channels_out);
  if (!output) return NULL;
  if (audio_mixer_process(mixer, input, output) != MIXER_OK) {
    audio_chunk_free(output);
    return NULL;
  }
  return output;
}

void audio_mixer_update_parameters(audio_mixer_t* mixer,
                                   const mixer_config_t* config) {
  if (!mixer || !config) return;
  // Free existing mapping source arrays before re-populating from new config
  for (size_t i = 0; i < mixer->channels_out; i++) {
    if (mixer->mapping[i].sources) {
      free(mixer->mapping[i].sources);
      mixer->mapping[i].sources = NULL;
    }
    mixer->mapping[i].count = 0;
  }
  populate_mapping(mixer, config);
}

void audio_mixer_free(audio_mixer_t* mixer) {
  if (!mixer) return;
  if (mixer->name) free(mixer->name);
  if (mixer->mapping) {
    for (size_t i = 0; i < mixer->channels_out; i++) {
      if (mixer->mapping[i].sources) {
        free(mixer->mapping[i].sources);
      }
    }
    free(mixer->mapping);
  }
  free(mixer);
}
