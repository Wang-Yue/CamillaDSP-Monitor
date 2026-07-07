#if defined(__linux__)
#define _GNU_SOURCE

#include "alsa_capabilities.h"
#include "alsa_device.h"

#include <alloca.h>
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const int ALSA_PROBE_RATES[] = {5512,   8000,   11025,  16000,  22050, 32000,
                                44100,  48000,  64000,  88200,  96000, 176400,
                                192000, 352800, 384000, 705600, 768000};
const size_t ALSA_PROBE_RATES_COUNT =
    sizeof(ALSA_PROBE_RATES) / sizeof(ALSA_PROBE_RATES[0]);

int alsa_capabilities_available_device_names(bool is_capture,
                                             char out_names[][256],
                                             int max_names) {
  (void)is_capture;
  int count = 0;
  if (count < max_names) {
    snprintf(out_names[count++], 256, "default");
  }
  int card_idx = -1;
  while (snd_card_next(&card_idx) == 0 && card_idx >= 0 && count < max_names) {
    char name[32];
    snprintf(name, sizeof(name), "hw:%d", card_idx);
    char* card_name = NULL;
    if (snd_card_get_name(card_idx, &card_name) == 0) {
      snprintf(out_names[count++], 256, "%s (%s)", name, card_name);
      free(card_name);
    } else {
      snprintf(out_names[count++], 256, "%s", name);
    }
  }
  return count;
}

bool alsa_capabilities_default_device_name(bool is_capture, char* out_name,
                                           size_t max_len) {
  (void)is_capture;
  snprintf(out_name, max_len, "default");
  return true;
}

int alsa_capabilities_channel_count(const char* device_name, bool is_capture) {
  pthread_mutex_lock(&g_alsa_mutex);
  snd_pcm_t* pcm = NULL;
  snd_pcm_stream_t stream =
      is_capture ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;
  char clean_name[256];
  snprintf(clean_name, sizeof(clean_name), "%s", device_name);
  char* space = strchr(clean_name, ' ');
  if (space) *space = '\0';

  int err = snd_pcm_open(&pcm, clean_name, stream, SND_PCM_NONBLOCK);
  if (err < 0) {
    pthread_mutex_unlock(&g_alsa_mutex);
    return 2;  // fallback default
  }

  snd_pcm_hw_params_t* params = NULL;
  snd_pcm_hw_params_alloca(&params);
  if (snd_pcm_hw_params_any(pcm, params) < 0) {
    snd_pcm_close(pcm);
    pthread_mutex_unlock(&g_alsa_mutex);
    return 2;
  }
  unsigned int max_ch = 2;
  snd_pcm_hw_params_get_channels_max(params, &max_ch);
  snd_pcm_close(pcm);
  pthread_mutex_unlock(&g_alsa_mutex);
  return (int)max_ch;
}

audio_device_descriptor_t* alsa_capabilities_describe(const char* device_name,
                                                      bool is_capture) {
  pthread_mutex_lock(&g_alsa_mutex);
  audio_device_descriptor_t* desc =
      (audio_device_descriptor_t*)calloc(1, sizeof(audio_device_descriptor_t));
  if (!desc) {
    pthread_mutex_unlock(&g_alsa_mutex);
    return NULL;
  }
  snprintf(desc->name, sizeof(desc->name), "%s", device_name);

  char clean_name[256];
  snprintf(clean_name, sizeof(clean_name), "%s", device_name);
  char* space = strchr(clean_name, ' ');
  if (space) *space = '\0';

  snd_pcm_stream_t stream =
      is_capture ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;
  snd_pcm_t* pcm = NULL;
  if (snd_pcm_open(&pcm, clean_name, stream, SND_PCM_NONBLOCK) < 0) {
    goto error_cleanup;
  }

  snd_pcm_hw_params_t* params = NULL;
  snd_pcm_hw_params_alloca(&params);
  if (snd_pcm_hw_params_any(pcm, params) < 0) {
    goto error_cleanup;
  }

  unsigned int min_ch = 1, max_ch = 2;
  snd_pcm_hw_params_get_channels_min(params, &min_ch);
  snd_pcm_hw_params_get_channels_max(params, &max_ch);

  // Let's create one capability set
  desc->capability_sets_count = 1;
  desc->capability_sets =
      (device_capability_set_t*)calloc(1, sizeof(device_capability_set_t));

  device_capability_set_t* set = &desc->capability_sets[0];
  // Probe channel sizes. If min_ch != max_ch, we report both or a range.
  // Let's create capabilities for all channel counts from min_ch to max_ch (up
  // to 32)
  size_t cap_idx = 0;
  size_t cap_alloc = (max_ch - min_ch + 1);
  set->capabilities =
      (channel_capability_t*)calloc(cap_alloc, sizeof(channel_capability_t));

  for (unsigned int ch = min_ch; ch <= max_ch; ch++) {
    // Test if this channel count is supported

    channel_capability_t* cap = &set->capabilities[cap_idx];
    cap->channels = (int)ch;

    // Build list of supported sample rates
    cap->samplerates = (samplerate_capability_t*)calloc(
        ALSA_PROBE_RATES_COUNT, sizeof(samplerate_capability_t));
    size_t rate_idx = 0;

    for (size_t r = 0; r < ALSA_PROBE_RATES_COUNT; r++) {
      int test_rate = ALSA_PROBE_RATES[r];
      // Test if test_rate is supported for this channel count
      snd_pcm_hw_params_any(pcm, params);
      snd_pcm_hw_params_set_channels(pcm, params, ch);
      if (snd_pcm_hw_params_set_rate(pcm, params, test_rate, 0) >= 0) {
        // Rate is supported! Now probe which formats are supported for this
        // rate and channel count
        samplerate_capability_t* rate_cap = &cap->samplerates[rate_idx];
        rate_cap->samplerate = test_rate;

        // Formats we support: S16_LE, S24_3_LE, S24_4_LE, S32_LE, F32_LE,
        // F64_LE
        const snd_pcm_format_t test_formats[] = {
            SND_PCM_FORMAT_S16_LE,   SND_PCM_FORMAT_S24_3LE,
            SND_PCM_FORMAT_S24_LE,   SND_PCM_FORMAT_S32_LE,
            SND_PCM_FORMAT_FLOAT_LE, SND_PCM_FORMAT_FLOAT64_LE};
        const char* format_names[] = {"S16_LE", "S24_3_LE", "S24_4_LE",
                                      "S32_LE", "F32_LE",   "F64_LE"};
        const size_t test_formats_count =
            sizeof(test_formats) / sizeof(test_formats[0]);

        rate_cap->formats = (char**)calloc(test_formats_count, sizeof(char*));
        size_t fmt_idx = 0;

        for (size_t f = 0; f < test_formats_count; f++) {
          snd_pcm_hw_params_any(pcm, params);
          snd_pcm_hw_params_set_channels(pcm, params, ch);
          snd_pcm_hw_params_set_rate(pcm, params, test_rate, 0);
          if (snd_pcm_hw_params_set_format(pcm, params, test_formats[f]) >= 0) {
            // Check if we already added this format name
            bool duplicate = false;
            for (size_t d = 0; d < fmt_idx; d++) {
              if (strcmp(rate_cap->formats[d], format_names[f]) == 0) {
                duplicate = true;
                break;
              }
            }
            if (!duplicate) {
              rate_cap->formats[fmt_idx++] = strdup(format_names[f]);
            }
          }
        }

        if (fmt_idx > 0) {
          rate_cap->formats_count = fmt_idx;
          rate_idx++;
        } else {
          free(rate_cap->formats);
          rate_cap->formats = NULL;
        }
      }
    }

    if (rate_idx > 0) {
      cap->samplerates_count = rate_idx;
      cap_idx++;
    } else {
      free(cap->samplerates);
      cap->samplerates = NULL;
    }
  }

  set->capabilities_count = cap_idx;
  snd_pcm_close(pcm);
  pthread_mutex_unlock(&g_alsa_mutex);
  return desc;

error_cleanup:
  if (pcm) {
    snd_pcm_close(pcm);
  }
  if (desc) {
    alsa_capabilities_free_descriptor(desc);
  }
  pthread_mutex_unlock(&g_alsa_mutex);
  return NULL;
}

void alsa_capabilities_free_descriptor(audio_device_descriptor_t* desc) {
  if (!desc) return;
  if (desc->capability_sets) {
    for (size_t s = 0; s < desc->capability_sets_count; s++) {
      device_capability_set_t* set = &desc->capability_sets[s];
      if (set->capabilities) {
        for (size_t c = 0; c < set->capabilities_count; c++) {
          channel_capability_t* ch_cap = &set->capabilities[c];
          if (ch_cap->samplerates) {
            for (size_t r = 0; r < ch_cap->samplerates_count; r++) {
              samplerate_capability_t* rate_cap = &ch_cap->samplerates[r];
              if (rate_cap->formats) {
                for (size_t f = 0; f < rate_cap->formats_count; f++) {
                  free(rate_cap->formats[f]);
                }
                free(rate_cap->formats);
              }
            }
            free(ch_cap->samplerates);
          }
        }
        free(set->capabilities);
      }
    }
    free(desc->capability_sets);
  }
  free(desc);
}

#endif  // defined(__linux__)
