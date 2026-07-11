#if defined(ENABLE_ALSA)
#include "alsa_capabilities.h"

#include <alloca.h>
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alsa_device.h"

const int ALSA_PROBE_RATES[] = {5512,   8000,   11025,  16000,  22050, 32000,
                                44100,  48000,  64000,  88200,  96000, 176400,
                                192000, 352800, 384000, 705600, 768000};
const size_t ALSA_PROBE_RATES_COUNT =
    sizeof(ALSA_PROBE_RATES) / sizeof(ALSA_PROBE_RATES[0]);

int alsa_capabilities_available_device_names(bool is_capture,
                                             char out_names[][256],
                                             int max_names) {
  (void)is_capture;
  pthread_mutex_lock(&g_alsa_mutex);
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
  pthread_mutex_unlock(&g_alsa_mutex);
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
  // Parse device name: strip any extra info like "(card description)"
  char clean_name[256];
  snprintf(clean_name, sizeof(clean_name), "%s", device_name);
  char* space = strchr(clean_name, ' ');
  if (space) *space = '\0';

  // Open the PCM device in non-blocking mode to probe capabilities
  int err = snd_pcm_open(&pcm, clean_name, stream, SND_PCM_NONBLOCK);
  if (err < 0) {
    pthread_mutex_unlock(&g_alsa_mutex);
    return 2;  // fallback default
  }

  snd_pcm_hw_params_t* params = NULL;
  snd_pcm_hw_params_alloca(&params);
  // Initialize hardware parameters structure
  if (snd_pcm_hw_params_any(pcm, params) < 0) {
    snd_pcm_close(pcm);
    pthread_mutex_unlock(&g_alsa_mutex);
    return 2;
  }
  // Retrieve maximum supported channels
  unsigned int max_ch = 2;
  snd_pcm_hw_params_get_channels_max(params, &max_ch);
  snd_pcm_close(pcm);
  pthread_mutex_unlock(&g_alsa_mutex);
  return (int)max_ch;
}

audio_device_descriptor_t* alsa_capabilities_describe(const char* device_name,
                                                      bool is_capture,
                                                      device_error_t* err) {
  pthread_mutex_lock(&g_alsa_mutex);
  audio_device_descriptor_t* desc =
      (audio_device_descriptor_t*)calloc(1, sizeof(audio_device_descriptor_t));
  if (!desc) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER, "Out of memory");
    }
    pthread_mutex_unlock(&g_alsa_mutex);
    return NULL;
  }
  snprintf(desc->name, sizeof(desc->name), "%s", device_name);

  // Parse device name to get actual ALSA device string (e.g. "hw:0")
  char clean_name[256];
  snprintf(clean_name, sizeof(clean_name), "%s", device_name);
  char* space = strchr(clean_name, ' ');
  if (space) *space = '\0';

  snd_pcm_stream_t stream =
      is_capture ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;
  snd_pcm_t* pcm = NULL;
  // Open device in non-blocking mode to avoid hangs
  int open_res = snd_pcm_open(&pcm, clean_name, stream, SND_PCM_NONBLOCK);
  if (open_res < 0) {
    if (err) {
      if (open_res == -EBUSY) {
        device_error_init(err, DEVICE_ERROR_BUSY, "Device or resource busy");
      } else if (open_res == -ENOENT || open_res == -ENODEV) {
        device_error_init(err, DEVICE_ERROR_NOT_FOUND, "Device not found");
      } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "ALSA open failed: %s",
                 snd_strerror(open_res));
        device_error_init(err, DEVICE_ERROR_OTHER, msg);
      }
    }
    goto error_cleanup;
  }

  snd_pcm_hw_params_t* params = NULL;
  snd_pcm_hw_params_alloca(&params);
  // Get full capability space
  if (snd_pcm_hw_params_any(pcm, params) < 0) {
    goto error_cleanup;
  }

  unsigned int min_ch = 1, max_ch = 2;
  snd_pcm_hw_params_get_channels_min(params, &min_ch);
  snd_pcm_hw_params_get_channels_max(params, &max_ch);
  if (min_ch > max_ch || min_ch == 0 || max_ch == 0) {
    min_ch = 1;
    max_ch = 2;
  }

  // Allocate capability set. We represent ALSA capabilities in one set.
  desc->capability_sets_count = 1;
  desc->capability_sets =
      (device_capability_set_t*)calloc(1, sizeof(device_capability_set_t));
  if (!desc->capability_sets) goto error_cleanup;

  device_capability_set_t* set = &desc->capability_sets[0];
  // Probe channel sizes by testing constraints.
  // We allocate space for every channel count from min to max.
  size_t cap_idx = 0;
  size_t cap_alloc = (max_ch - min_ch + 1);
  set->capabilities =
      (channel_capability_t*)calloc(cap_alloc, sizeof(channel_capability_t));
  if (!set->capabilities) goto error_cleanup;
  set->capabilities_count = cap_alloc;

  for (unsigned int ch = min_ch; ch <= max_ch; ch++) {
    channel_capability_t* cap = &set->capabilities[cap_idx];
    cap->channels = (int)ch;

    // Allocate memory for sample rates to probe. We probe from the static list
    // ALSA_PROBE_RATES.
    cap->samplerates = (samplerate_capability_t*)calloc(
        ALSA_PROBE_RATES_COUNT, sizeof(samplerate_capability_t));
    if (!cap->samplerates) goto error_cleanup;
    cap->samplerates_count = ALSA_PROBE_RATES_COUNT;
    size_t rate_idx = 0;

    for (size_t r = 0; r < ALSA_PROBE_RATES_COUNT; r++) {
      int test_rate = ALSA_PROBE_RATES[r];
      // Reset hw params to full space and constrain channels first, then test
      // sample rate support
      snd_pcm_hw_params_any(pcm, params);
      snd_pcm_hw_params_set_channels(pcm, params, ch);
      if (snd_pcm_hw_params_set_rate(pcm, params, test_rate, 0) >= 0) {
        // Sample rate supported for this channel count. Now probe formats.
        samplerate_capability_t* rate_cap = &cap->samplerates[rate_idx];
        rate_cap->samplerate = test_rate;

        // Formats we want to probe
        const snd_pcm_format_t test_formats[] = {
            SND_PCM_FORMAT_S16_LE,   SND_PCM_FORMAT_S24_3LE,
            SND_PCM_FORMAT_S24_LE,   SND_PCM_FORMAT_S32_LE,
            SND_PCM_FORMAT_FLOAT_LE, SND_PCM_FORMAT_FLOAT64_LE};
        const char* format_names[] = {"S16_LE", "S24_3_LE", "S24_4_LE",
                                      "S32_LE", "F32_LE",   "F64_LE"};
        const size_t test_formats_count =
            sizeof(test_formats) / sizeof(test_formats[0]);

        rate_cap->formats = (char**)calloc(test_formats_count, sizeof(char*));
        if (!rate_cap->formats) goto error_cleanup;
        rate_cap->formats_count = test_formats_count;
        size_t fmt_idx = 0;

        for (size_t f = 0; f < test_formats_count; f++) {
          // Reset params and constrain channel -> rate -> format
          snd_pcm_hw_params_any(pcm, params);
          snd_pcm_hw_params_set_channels(pcm, params, ch);
          snd_pcm_hw_params_set_rate(pcm, params, test_rate, 0);
          if (snd_pcm_hw_params_set_format(pcm, params, test_formats[f]) >= 0) {
            // Deduplicate format names (just in case)
            bool duplicate = false;
            for (size_t d = 0; d < fmt_idx; d++) {
              if (strcmp(rate_cap->formats[d], format_names[f]) == 0) {
                duplicate = true;
                break;
              }
            }
            if (!duplicate) {
              char* fmt_str = strdup(format_names[f]);
              if (!fmt_str) goto error_cleanup;
              rate_cap->formats[fmt_idx++] = fmt_str;
            }
          }
        }

        if (fmt_idx > 0) {
          rate_cap->formats_count = fmt_idx;
          rate_idx++;
        } else {
          free(rate_cap->formats);
          rate_cap->formats = NULL;
          rate_cap->formats_count = 0;
        }
      }
    }

    if (rate_idx > 0) {
      cap->samplerates_count = rate_idx;
      cap_idx++;
    } else {
      free(cap->samplerates);
      cap->samplerates = NULL;
      cap->samplerates_count = 0;
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
    free_audio_device_descriptor(desc);
  }
  pthread_mutex_unlock(&g_alsa_mutex);
  return NULL;
}

#endif  // defined(ENABLE_ALSA)
