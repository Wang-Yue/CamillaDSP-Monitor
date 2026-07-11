// Device capability discovery for CoreAudio.

#include "core_audio_capabilities.h"
#if defined(ENABLE_COREAUDIO)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MARK: - Discovery

/// Sample rates we report when a device exposes a *range* rather than a
/// discrete list. CoreAudio devices commonly advertise something like
/// 44.1 kHz – 192 kHz; we report only the standard rates that fall
/// inside the range so the UI doesn't need to render thousands of
/// values.
///
/// Public so room-correction tooling can pre-render an FIR per
/// rate, then pick the matching one at engine-config time.
const int CORE_AUDIO_STANDARD_RATES[15] = {
    8000,  11025,  16000,  22050,  32000,  44100,  48000, 88200,
    96000, 176400, 192000, 352800, 384000, 705600, 768000};
const size_t CORE_AUDIO_STANDARD_RATES_COUNT = 15;

// MARK: Device enumeration
//
// Thin wrappers over `CoreAudioDevice` so the UI doesn't need to
// touch HAL types. Anything beyond a name lives in the capability
// descriptor (`describe`) below.

/// Names of all devices visible to the system in the requested
/// direction. Empty when no devices match (no mics connected, no
/// output devices, etc.).
int core_audio_capabilities_available_device_names(bool is_capture,
                                                   char out_names[][256],
                                                   int max_names) {
  core_audio_scope_t scope =
      is_capture ? CORE_AUDIO_SCOPE_INPUT : CORE_AUDIO_SCOPE_OUTPUT;
  core_audio_device_info_t devices[128];
  int count = core_audio_device_list_devices(scope, devices, 128);
  int res = 0;
  for (int i = 0; i < count && res < max_names; i++) {
    if (devices[i].name[0] != '\0') {
      strncpy(out_names[res], devices[i].name, 255);
      out_names[res][255] = '\0';
      res++;
    }
  }
  return res;
}

/// Name of the system-default device in the requested direction,
/// if one is configured. Useful as the initial value for a picker.
bool core_audio_capabilities_default_device_name(bool is_capture,
                                                 char* out_name,
                                                 size_t max_len) {
  core_audio_scope_t scope =
      is_capture ? CORE_AUDIO_SCOPE_INPUT : CORE_AUDIO_SCOPE_OUTPUT;
  AudioDeviceID id = core_audio_device_default_id(scope);
  if (id == 0) return false;
  return core_audio_device_name(id, out_name, max_len);
}

/// Maximum channel count the named device exposes across any of
/// its physical formats. When `name` is `nil` the system default
/// is queried. Returns `0` if the device can't be located.
///
/// Derived from `describe(deviceName:isCapture:)` — no separate HAL
/// query — so the answer matches whatever the capability descriptor
/// reports. Used by the room-correction UI to populate per-channel
/// pickers (e.g. left/right speaker, calibrated mic capsule on a
/// stereo interface).
int core_audio_capabilities_channel_count(const char* device_name,
                                          bool is_capture) {
  audio_device_descriptor_t* desc =
      core_audio_capabilities_describe(device_name, is_capture, NULL);
  if (!desc) return 0;
  int max_ch = 0;
  for (size_t s = 0; s < desc->capability_sets_count; s++) {
    device_capability_set_t* set = &desc->capability_sets[s];
    for (size_t c = 0; c < set->capabilities_count; c++) {
      if (set->capabilities[c].channels > max_ch) {
        max_ch = set->capabilities[c].channels;
      }
    }
  }
  free_audio_device_descriptor(desc);
  return max_ch;
}

// MARK: CoreAudio plumbing

/// One physical format entry from
/// `kAudioStreamPropertyAvailablePhysicalFormats`. `samplerates` is the list of
/// standard rates that fit inside the AudioStreamRangedDescription range
/// (typically a single value, but some devices report a range).
typedef struct {
  int channels;
  int samplerate;
  char format[16];
} phys_fmt_t;

/// Map an AudioStreamBasicDescription to a DSP CoreAudio sample
/// format token (S16, S24, S32, F32) — exactly the formats the CoreAudio
/// backend accepts. Anything else (e.g. 64-bit float, unsigned PCM)
/// returns an empty string and is filtered out by the caller.
/**
 * @brief Map CoreAudio AudioStreamBasicDescription to a string representing the
 * sample format.
 *
 * This helper filters formats and maps supported formats to "S16", "S24",
 * "S32", or "F32". Unsupported formats return an empty string.
 *
 * @param asbd Pointer to the AudioStreamBasicDescription to inspect.
 * @return A string literal ("S16", "S24", "S32", "F32") or an empty string if
 * unsupported.
 */
static const char* format_string_for_asbd(
    const AudioStreamBasicDescription* asbd) {
  AudioFormatFlags flags = asbd->mFormatFlags;
  bool is_float = (flags & kAudioFormatFlagIsFloat) != 0;
  bool is_signed_int = (flags & kAudioFormatFlagIsSignedInteger) != 0;
  uint32_t bits = asbd->mBitsPerChannel;

  if (is_float && bits == 32) return "F32";
  if (is_signed_int) {
    switch (bits) {
      case 16:
        return "S16";
      case 24:
        return "S24";
      case 32:
        return "S32";
      default:
        return "";
    }
  }
  return "";
}

// MARK: Aggregation

/// Build the capability descriptor for a named device. Returns `nil`
/// if the device cannot be located. All low-level HAL plumbing is
/// delegated to `CoreAudioDevice`; this layer only adds the
/// physical-format probe + aggregation that's specific to the UI's
/// `AudioDeviceDescriptor` shape.
/// Group `(channels, samplerate, format)` tuples into the nested shape
/// the UI consumes: channels → samplerates → formats.
/// Walk every `AudioStreamRangedDescription` advertised by `stream`
/// and translate it into our `PhysicalFormat`.
/// Pick standard sample rates that fall inside the device's range.
/// Devices that advertise a single discrete rate report
/// `mMinimum == mMaximum`; in that case we return the single value
/// (rounded to `Int`).
audio_device_descriptor_t* core_audio_capabilities_describe(
    const char* device_name, bool is_capture, device_error_t* err) {
  core_audio_scope_t scope =
      is_capture ? CORE_AUDIO_SCOPE_INPUT : CORE_AUDIO_SCOPE_OUTPUT;
  // Look up the internal HAL AudioDeviceID for the given device name.
  AudioDeviceID id = core_audio_device_id_for_name(device_name, scope);
  if (id == 0) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_NOT_FOUND, "Device not found");
    }
    return NULL;
  }

  // Allocate the wrapper descriptor structure.
  audio_device_descriptor_t* desc =
      (audio_device_descriptor_t*)calloc(1, sizeof(audio_device_descriptor_t));
  if (!desc) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER, "Out of memory");
    }
    return NULL;
  }

  // Get the actual device name from HAL.
  if (!core_audio_device_name(id, desc->name, sizeof(desc->name))) {
    if (device_name) {
      strncpy(desc->name, device_name, sizeof(desc->name) - 1);
    }
  }

  // Query the list of AudioStreamIDs that belong to this device.
  AudioStreamID streams[32];
  int stream_count = core_audio_device_streams(id, scope, streams, 32);

  // Temporary flat array to collect formats across all streams.
  phys_fmt_t fmts[256] = {0};
  int fmt_count = 0;

  // Iterate through each stream to probe its physical formats.
  for (int s = 0; s < stream_count; s++) {
    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioStreamPropertyAvailablePhysicalFormats,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain};
    uint32_t size = 0;
    // Query the size of the kAudioStreamPropertyAvailablePhysicalFormats array.
    if (AudioObjectGetPropertyDataSize(streams[s], &addr, 0, NULL, &size) !=
            noErr ||
        size == 0)
      continue;
    int count = (int)(size / sizeof(AudioStreamRangedDescription));
    AudioStreamRangedDescription* ranged =
        (AudioStreamRangedDescription*)calloc(
            count, sizeof(AudioStreamRangedDescription));
    if (!ranged) continue;
    // Fetch the actual physical formats array.
    if (AudioObjectGetPropertyData(streams[s], &addr, 0, NULL, &size, ranged) ==
        noErr) {
      for (int i = 0; i < count; i++) {
        AudioStreamBasicDescription asbd = ranged[i].mFormat;
        // We only support Linear PCM.
        if (asbd.mFormatID != kAudioFormatLinearPCM) continue;
        const char* fmt_str = format_string_for_asbd(&asbd);
        if (fmt_str[0] == '\0') continue;

        double lo = ranged[i].mSampleRateRange.mMinimum;
        double hi = ranged[i].mSampleRateRange.mMaximum;

        int rates_to_add[32];
        int rate_cnt = 0;
        // Resolve sample rates: if it's a fixed value (lo == hi), add that.
        // If it's a range, intersect it with our list of standard rates
        // (CORE_AUDIO_STANDARD_RATES).
        if (lo == hi) {
          rates_to_add[rate_cnt++] = (int)round(lo);
        } else {
          for (size_t r = 0; r < CORE_AUDIO_STANDARD_RATES_COUNT; r++) {
            if ((double)CORE_AUDIO_STANDARD_RATES[r] >= lo &&
                (double)CORE_AUDIO_STANDARD_RATES[r] <= hi) {
              rates_to_add[rate_cnt++] = CORE_AUDIO_STANDARD_RATES[r];
            }
          }
          // Also check if the current nominal format sample rate is inside
          // the range and not already in our list.
          int hint = (int)round(asbd.mSampleRate);
          if (hint > 0) {
            bool found = false;
            for (int k = 0; k < rate_cnt; k++) {
              if (rates_to_add[k] == hint) {
                found = true;
                break;
              }
            }
            if (!found && rate_cnt < 32) {
              rates_to_add[rate_cnt++] = hint;
            }
          }
        }

        // Store combinations in the flat array.
        for (int r = 0; r < rate_cnt; r++) {
          if (fmt_count < 256) {
            fmts[fmt_count].channels = (int)asbd.mChannelsPerFrame;
            fmts[fmt_count].samplerate = rates_to_add[r];
            strncpy(fmts[fmt_count].format, fmt_str, 15);
            fmt_count++;
          }
        }
      }
    }
    free(ranged);
  }

  // Aggregate the flat array into the hierarchically nested capability tree
  // structure: Channel counts -> Sample rates -> Formats.
  desc->capability_sets =
      (device_capability_set_t*)calloc(1, sizeof(device_capability_set_t));
  if (!desc->capability_sets) {
    free_audio_device_descriptor(desc);
    return NULL;
  }
  desc->capability_sets_count = 1;

  // Find unique channel counts present in the collected formats.
  int unique_ch[32];
  int unique_ch_cnt = 0;
  for (int i = 0; i < fmt_count; i++) {
    bool found = false;
    for (int j = 0; j < unique_ch_cnt; j++) {
      if (unique_ch[j] == fmts[i].channels) {
        found = true;
        break;
      }
    }
    if (!found && unique_ch_cnt < 32) {
      unique_ch[unique_ch_cnt++] = fmts[i].channels;
    }
  }

  device_capability_set_t* set = &desc->capability_sets[0];
  set->capabilities = (channel_capability_t*)calloc(
      unique_ch_cnt, sizeof(channel_capability_t));
  if (!set->capabilities) {
    free_audio_device_descriptor(desc);
    return NULL;
  }
  set->capabilities_count = unique_ch_cnt;

  // For each unique channel count, find all the unique sample rates.
  for (int c = 0; c < unique_ch_cnt; c++) {
    channel_capability_t* ch_cap = &set->capabilities[c];
    ch_cap->channels = unique_ch[c];

    int unique_rate[64];
    int unique_rate_cnt = 0;
    for (int i = 0; i < fmt_count; i++) {
      if (fmts[i].channels == ch_cap->channels) {
        bool found = false;
        for (int j = 0; j < unique_rate_cnt; j++) {
          if (unique_rate[j] == fmts[i].samplerate) {
            found = true;
            break;
          }
        }
        if (!found && unique_rate_cnt < 64) {
          unique_rate[unique_rate_cnt++] = fmts[i].samplerate;
        }
      }
    }

    ch_cap->samplerates = (samplerate_capability_t*)calloc(
        unique_rate_cnt, sizeof(samplerate_capability_t));
    if (!ch_cap->samplerates) {
      free_audio_device_descriptor(desc);
      return NULL;
    }
    ch_cap->samplerates_count = unique_rate_cnt;

    // For each combination of channel count and sample rate, extract the unique
    // formats.
    for (int r = 0; r < unique_rate_cnt; r++) {
      samplerate_capability_t* rate_cap = &ch_cap->samplerates[r];
      rate_cap->samplerate = unique_rate[r];

      char unique_fmt[16][16] = {0};
      int unique_fmt_cnt = 0;
      for (int i = 0; i < fmt_count; i++) {
        if (fmts[i].channels == ch_cap->channels &&
            fmts[i].samplerate == rate_cap->samplerate) {
          bool found = false;
          for (int j = 0; j < unique_fmt_cnt; j++) {
            if (strcmp(unique_fmt[j], fmts[i].format) == 0) {
              found = true;
              break;
            }
          }
          if (!found && unique_fmt_cnt < 16) {
            strncpy(unique_fmt[unique_fmt_cnt++], fmts[i].format, 15);
          }
        }
      }

      rate_cap->formats = (char**)calloc(unique_fmt_cnt, sizeof(char*));
      if (!rate_cap->formats) {
        free_audio_device_descriptor(desc);
        return NULL;
      }
      rate_cap->formats_count = unique_fmt_cnt;
      for (int f = 0; f < unique_fmt_cnt; f++) {
        rate_cap->formats[f] = strdup(unique_fmt[f]);
        if (!rate_cap->formats[f]) {
          free_audio_device_descriptor(desc);
          return NULL;
        }
      }
    }
  }

  return desc;
}

#endif  // ENABLE_COREAUDIO
