#if defined(ENABLE_WASAPI)

#define WIN32_LEAN_AND_MEAN
#include "wasapi_capabilities.h"

#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "Logging/app_logger.h"

#define SAFE_RELEASE(punk)         \
  if ((punk) != NULL) {            \
    (punk)->lpVtbl->Release(punk); \
    (punk) = NULL;                 \
  }

int wasapi_capabilities_available_device_names(bool is_capture,
                                               char out_names[][256],
                                               int max_names) {
  HRESULT init_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  bool com_initialized = SUCCEEDED(init_hr);

  IMMDeviceEnumerator* enumerator = NULL;
  HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator, (void**)&enumerator);
  if (FAILED(hr)) goto error_cleanup;

  IMMDeviceCollection* collection = NULL;
  hr = IMMDeviceEnumerator_EnumAudioEndpoints(enumerator,
                                              is_capture ? eCapture : eRender,
                                              DEVICE_STATE_ACTIVE, &collection);
  if (FAILED(hr)) {
    goto error_cleanup;
  }

  UINT count = 0;
  IMMDeviceCollection_GetCount(collection, &count);
  int matched = 0;

  // Add default device first
  if (matched < max_names) {
    snprintf(out_names[matched++], 256, "default");
  }

  for (UINT i = 0; i < count && matched < max_names; i++) {
    IMMDevice* dev = NULL;
    IMMDeviceCollection_Item(collection, i, &dev);
    if (dev) {
      IPropertyStore* properties = NULL;
      char name_buf[256] = {0};
      bool has_name = false;
      HRESULT hr_prop =
          IMMDevice_OpenPropertyStore(dev, STGM_READ, &properties);
      if (SUCCEEDED(hr_prop)) {
        PROPVARIANT var;
        PropVariantInit(&var);
        hr_prop = IPropertyStore_GetValue(properties, &PKEY_Device_FriendlyName,
                                          &var);
        if (SUCCEEDED(hr_prop) && var.vt == VT_LPWSTR) {
          wcstombs(name_buf, var.pwszVal, sizeof(name_buf) - 1);
          name_buf[sizeof(name_buf) - 1] = '\0';
          has_name = true;
          PropVariantClear(&var);
        }
        SAFE_RELEASE(properties);
      }
      if (!has_name) {
        LPWSTR id = NULL;
        IMMDevice_GetId(dev, &id);
        if (id) {
          wcstombs(name_buf, id, sizeof(name_buf) - 1);
          name_buf[sizeof(name_buf) - 1] = '\0';
          CoTaskMemFree(id);
        }
      }
      if (name_buf[0] != '\0') {
        snprintf(out_names[matched++], 256, "%s", name_buf);
      }
      IMMDevice_Release(dev);
    }
  }

  SAFE_RELEASE(collection);
  SAFE_RELEASE(enumerator);
  if (com_initialized) {
    CoUninitialize();
  }
  return matched;

error_cleanup:
  if (collection) {
    SAFE_RELEASE(collection);
  }
  if (enumerator) {
    SAFE_RELEASE(enumerator);
  }
  if (com_initialized) {
    CoUninitialize();
  }
  return 0;
}

bool wasapi_capabilities_default_device_name(bool is_capture, char* out_name,
                                             size_t max_len) {
  (void)is_capture;
  snprintf(out_name, max_len, "default");
  return true;
}

int wasapi_capabilities_channel_count(const char* device_name,
                                      bool is_capture) {
  (void)device_name;
  (void)is_capture;
  return 2;  // Default fallback
}

audio_device_descriptor_t* wasapi_capabilities_describe(const char* device_name,
                                                        bool is_capture,
                                                        device_error_t* err) {
  HRESULT init_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  bool com_initialized = SUCCEEDED(init_hr);

  IMMDeviceEnumerator* enumerator = NULL;
  IMMDevice* device = NULL;
  IAudioClient* client = NULL;
  audio_device_descriptor_t* desc = NULL;

  HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator, (void**)&enumerator);
  if (FAILED(hr)) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER,
                        "Failed to create MMDeviceEnumerator");
    }
    goto error_cleanup;
  }

  device = wasapi_find_device_by_name(enumerator, device_name, is_capture);

  if (!device) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_NOT_FOUND, "Device not found");
    }
    goto error_cleanup;
  }

  hr = IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL,
                          (void**)&client);
  if (FAILED(hr)) {
    if (err) {
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        device_error_init(err, DEVICE_ERROR_NOT_FOUND, "Device invalidated");
      } else if (hr == E_ACCESSDENIED ||
                 hr == AUDCLNT_E_RESOURCES_INVALIDATED) {
        device_error_init(err, DEVICE_ERROR_BUSY, "Device is busy");
      } else {
        device_error_init(err, DEVICE_ERROR_OTHER,
                          "Failed to activate WASAPI client");
      }
    }
    goto error_cleanup;
  }

  desc =
      (audio_device_descriptor_t*)calloc(1, sizeof(audio_device_descriptor_t));
  if (!desc) {
    goto error_cleanup;
  }

  snprintf(desc->name, sizeof(desc->name), "%s", device_name);

  // Define the set of channels, rates, and formats we want to probe.
  // We use this trial-and-error approach because WASAPI does not provide
  // a direct API to query all supported configurations.
  const int PROBE_CHANNELS[] = {2, 8};
  const size_t PROBE_CHANNELS_COUNT =
      sizeof(PROBE_CHANNELS) / sizeof(PROBE_CHANNELS[0]);

  const int PROBE_RATES[] = {44100, 48000, 88200, 96000, 192000};
  const size_t PROBE_RATES_COUNT = sizeof(PROBE_RATES) / sizeof(PROBE_RATES[0]);

  const char* PROBE_FORMAT_NAMES[] = {"S16", "S24", "S32", "F32"};
  const wasapi_sample_format_t PROBE_FORMATS[] = {
      WASAPI_SAMPLE_FORMAT_S16, WASAPI_SAMPLE_FORMAT_S24,
      WASAPI_SAMPLE_FORMAT_S32, WASAPI_SAMPLE_FORMAT_F32};
  const size_t PROBE_FORMATS_COUNT =
      sizeof(PROBE_FORMATS) / sizeof(PROBE_FORMATS[0]);

  desc->capability_sets_count = 1;
  desc->capability_sets =
      (device_capability_set_t*)calloc(1, sizeof(device_capability_set_t));
  if (!desc->capability_sets) {
    free_audio_device_descriptor(desc);
    goto error_cleanup;
  }

  device_capability_set_t* set = &desc->capability_sets[0];
  set->capabilities = (channel_capability_t*)calloc(
      PROBE_CHANNELS_COUNT, sizeof(channel_capability_t));
  if (!set->capabilities) {
    free_audio_device_descriptor(desc);
    goto error_cleanup;
  }

  size_t valid_channels_count = 0;

  // Probe each channel count.
  for (size_t c_idx = 0; c_idx < PROBE_CHANNELS_COUNT; c_idx++) {
    int channels = PROBE_CHANNELS[c_idx];
    channel_capability_t* chan_cap = &set->capabilities[valid_channels_count];
    chan_cap->channels = channels;
    chan_cap->samplerates = (samplerate_capability_t*)calloc(
        PROBE_RATES_COUNT, sizeof(samplerate_capability_t));
    if (!chan_cap->samplerates) {
      free_audio_device_descriptor(desc);
      goto error_cleanup;
    }

    size_t valid_rates_count = 0;

    // Probe each sample rate for the current channel count.
    for (size_t r_idx = 0; r_idx < PROBE_RATES_COUNT; r_idx++) {
      int rate = PROBE_RATES[r_idx];
      samplerate_capability_t* rate_cap =
          &chan_cap->samplerates[valid_rates_count];
      rate_cap->samplerate = rate;
      rate_cap->formats = (char**)calloc(PROBE_FORMATS_COUNT, sizeof(char*));
      if (!rate_cap->formats) {
        free_audio_device_descriptor(desc);
        goto error_cleanup;
      }

      size_t valid_formats_count = 0;

      // Probe each sample format for the current channel count and sample rate.
      for (size_t f_idx = 0; f_idx < PROBE_FORMATS_COUNT; f_idx++) {
        wasapi_sample_format_t fmt = PROBE_FORMATS[f_idx];

        // Configure WAVEFORMATEXTENSIBLE for probing.
        WAVEFORMATEXTENSIBLE wfx;
        memset(&wfx, 0, sizeof(wfx));
        wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfx.Format.nChannels = channels;
        wfx.Format.nSamplesPerSec = rate;
        wfx.Format.wBitsPerSample = (fmt == WASAPI_SAMPLE_FORMAT_S16) ? 16 : 32;
        wfx.Format.nBlockAlign = (wfx.Format.wBitsPerSample / 8) * channels;
        wfx.Format.nAvgBytesPerSec =
            wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
        wfx.Format.cbSize = 22;
        wfx.Samples.wValidBitsPerSample =
            (fmt == WASAPI_SAMPLE_FORMAT_S24) ? 24 : wfx.Format.wBitsPerSample;
        wfx.dwChannelMask =
            (channels == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
        wfx.SubFormat = (fmt == WASAPI_SAMPLE_FORMAT_F32)
                            ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                            : KSDATAFORMAT_SUBTYPE_PCM;

        WAVEFORMATEX* closest = NULL;
        // Query WASAPI if the format is supported in shared mode.
        hr = IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_SHARED,
                                            (WAVEFORMATEX*)&wfx, &closest);
        if (closest) CoTaskMemFree(closest);

        if (SUCCEEDED(hr)) {
          rate_cap->formats[valid_formats_count++] =
              strdup(PROBE_FORMAT_NAMES[f_idx]);
        }
      }

      // If at least one format was supported, this rate is valid.
      if (valid_formats_count > 0) {
        rate_cap->formats_count = valid_formats_count;
        valid_rates_count++;
      } else {
        free(rate_cap->formats);
        rate_cap->formats = NULL;
      }
    }

    // If at least one rate was supported, this channel count is valid.
    if (valid_rates_count > 0) {
      chan_cap->samplerates_count = valid_rates_count;
      valid_channels_count++;
    } else {
      free(chan_cap->samplerates);
      chan_cap->samplerates = NULL;
    }
  }

  if (valid_channels_count > 0) {
    set->capabilities_count = valid_channels_count;
  } else {
    free_audio_device_descriptor(desc);
    desc = NULL;
  }

  SAFE_RELEASE(client);
  SAFE_RELEASE(device);
  SAFE_RELEASE(enumerator);
  if (com_initialized) {
    CoUninitialize();
  }
  return desc;

error_cleanup:
  if (client) {
    SAFE_RELEASE(client);
  }
  if (device) {
    SAFE_RELEASE(device);
  }
  if (enumerator) {
    SAFE_RELEASE(enumerator);
  }
  if (com_initialized) {
    CoUninitialize();
  }
  return NULL;
}

IMMDevice* wasapi_find_device_by_name(IMMDeviceEnumerator* enumerator,
                                      const char* device_name,
                                      bool is_capture) {
  HRESULT hr;
  IMMDevice* device = NULL;

  if (device_name[0] == '\0' || strcmp(device_name, "default") == 0) {
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
        enumerator, is_capture ? eCapture : eRender, eConsole, &device);
    if (SUCCEEDED(hr)) {
      return device;
    }
    return NULL;
  }

  IMMDeviceCollection* collection = NULL;
  hr = IMMDeviceEnumerator_EnumAudioEndpoints(enumerator,
                                              is_capture ? eCapture : eRender,
                                              DEVICE_STATE_ACTIVE, &collection);
  if (FAILED(hr)) return NULL;

  UINT count = 0;
  IMMDeviceCollection_GetCount(collection, &count);
  for (UINT i = 0; i < count; i++) {
    IMMDevice* dev = NULL;
    IMMDeviceCollection_Item(collection, i, &dev);
    bool matched = false;

    IPropertyStore* properties = NULL;
    HRESULT hr_prop = IMMDevice_OpenPropertyStore(dev, STGM_READ, &properties);
    if (SUCCEEDED(hr_prop)) {
      PROPVARIANT var;
      PropVariantInit(&var);
      hr_prop =
          IPropertyStore_GetValue(properties, &PKEY_Device_FriendlyName, &var);
      if (SUCCEEDED(hr_prop) && var.vt == VT_LPWSTR) {
        char friendly_name[256] = {0};
        wcstombs(friendly_name, var.pwszVal, sizeof(friendly_name) - 1);
        friendly_name[sizeof(friendly_name) - 1] = '\0';
        if (strstr(friendly_name, device_name) != NULL) {
          matched = true;
        }
        PropVariantClear(&var);
      }
      SAFE_RELEASE(properties);
    }

    if (!matched) {
      LPWSTR id = NULL;
      IMMDevice_GetId(dev, &id);
      if (id) {
        char dev_id_char[256];
        wcstombs(dev_id_char, id, sizeof(dev_id_char) - 1);
        dev_id_char[sizeof(dev_id_char) - 1] = '\0';
        if (strstr(dev_id_char, device_name) != NULL) {
          matched = true;
        }
        CoTaskMemFree(id);
      }
    }

    if (matched) {
      device = dev;
      break;
    }
    IMMDevice_Release(dev);
  }
  IMMDeviceCollection_Release(collection);
  return device;
}

#endif  // ENABLE_WASAPI
