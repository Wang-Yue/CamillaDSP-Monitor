#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include "wasapi_backend.h"

#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <initguid.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "Logging/app_logger.h"

// COM Release helper
#define SAFE_RELEASE(punk)         \
  if ((punk) != NULL) {            \
    (punk)->lpVtbl->Release(punk); \
    (punk) = NULL;                 \
  }

struct wasapi_capture {
  char device[256];
  int sample_rate;
  int channels;
  int chunk_size;
  wasapi_sample_format_t format;
  bool loopback;

  IMMDeviceEnumerator* enumerator;
  IMMDevice* mm_device;
  IAudioClient* client;
  IAudioCaptureClient* capture_client;
  UINT32 buffer_frame_count;
};

struct wasapi_playback {
  char device[256];
  int sample_rate;
  int channels;
  int chunk_size;
  wasapi_sample_format_t format;
  bool exclusive;

  IMMDeviceEnumerator* enumerator;
  IMMDevice* mm_device;
  IAudioClient* client;
  IAudioRenderClient* render_client;
  UINT32 buffer_frame_count;
  bool paused;
};

static size_t get_sample_size(wasapi_sample_format_t format) {
  switch (format) {
    case WASAPI_SAMPLE_FORMAT_S16:
      return 2;
    case WASAPI_SAMPLE_FORMAT_S24:
      return 4;  // 24-bit in 32-bit container
    case WASAPI_SAMPLE_FORMAT_S32:
      return 4;
    case WASAPI_SAMPLE_FORMAT_F32:
      return 4;
    default:
      return 0;
  }
}

static inline void decode_samples_from_wasapi(audio_chunk_t* chunk,
                                              size_t chunk_offset,
                                              const BYTE* src, size_t frames,
                                              wasapi_sample_format_t format,
                                              int channels, DWORD flags) {
  if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        audio_chunk_get_channel(chunk, c)[chunk_offset + f] = 0.0;
      }
    }
    return;
  }

  switch (format) {
    case WASAPI_SAMPLE_FORMAT_S16: {
      const int16_t* s16 = (const int16_t*)src;
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          audio_chunk_get_channel(chunk, c)[chunk_offset + f] =
              (double)s16[f * channels + c] / 32768.0;
        }
      }
      break;
    }
    case WASAPI_SAMPLE_FORMAT_S24: {
      // S24 is typically stored in the high 24 bits of 32-bit integers
      const int32_t* s32 = (const int32_t*)src;
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          int32_t val =
              s32[f * channels + c] >> 8;  // shift right to match 24-bit range
          audio_chunk_get_channel(chunk, c)[chunk_offset + f] =
              (double)val / 8388608.0;
        }
      }
      break;
    }
    case WASAPI_SAMPLE_FORMAT_S32: {
      const int32_t* s32 = (const int32_t*)src;
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          audio_chunk_get_channel(chunk, c)[chunk_offset + f] =
              (double)s32[f * channels + c] / 2147483648.0;
        }
      }
      break;
    }
    case WASAPI_SAMPLE_FORMAT_F32: {
      const float* f32 = (const float*)src;
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          audio_chunk_get_channel(chunk, c)[chunk_offset + f] =
              (double)f32[f * channels + c];
        }
      }
      break;
    }
    default:
      break;
  }
}

static inline void encode_samples_to_wasapi(BYTE* dst,
                                            const audio_chunk_t* chunk,
                                            size_t chunk_offset, size_t frames,
                                            wasapi_sample_format_t format,
                                            int channels) {
  switch (format) {
    case WASAPI_SAMPLE_FORMAT_S16: {
      int16_t* s16 = (int16_t*)dst;
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          double val = audio_chunk_get_channel(chunk, c)[chunk_offset + f];
          if (val > 1.0)
            val = 1.0;
          else if (val < -1.0)
            val = -1.0;
          s16[f * channels + c] = (int16_t)(val * 32767.0);
        }
      }
      break;
    }
    case WASAPI_SAMPLE_FORMAT_S24: {
      int32_t* s32 = (int32_t*)dst;
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          double val = audio_chunk_get_channel(chunk, c)[chunk_offset + f];
          if (val > 1.0)
            val = 1.0;
          else if (val < -1.0)
            val = -1.0;
          s32[f * channels + c] = ((int32_t)(val * 8388607.0)) << 8;
        }
      }
      break;
    }
    case WASAPI_SAMPLE_FORMAT_S32: {
      int32_t* s32 = (int32_t*)dst;
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          double val = audio_chunk_get_channel(chunk, c)[chunk_offset + f];
          if (val > 1.0)
            val = 1.0;
          else if (val < -1.0)
            val = -1.0;
          s32[f * channels + c] = (int32_t)(val * 2147483647.0);
        }
      }
      break;
    }
    case WASAPI_SAMPLE_FORMAT_F32: {
      float* f32 = (float*)dst;
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          f32[f * channels + c] =
              (float)audio_chunk_get_channel(chunk, c)[chunk_offset + f];
        }
      }
      break;
    }
    default:
      break;
  }
}

// MARK: - Capture Backend implementation

static bool cap_vtable_open(void* ctx, backend_error_t* err) {
  return wasapi_capture_open((wasapi_capture_t*)ctx, err);
}
static bool cap_vtable_read(void* ctx, size_t frames, audio_chunk_t* chunk,
                            backend_error_t* err) {
  return wasapi_capture_read((wasapi_capture_t*)ctx, frames, chunk, err);
}
static void cap_vtable_close(void* ctx) {
  wasapi_capture_close((wasapi_capture_t*)ctx);
}
static bool cap_vtable_get_pending_rate_change(void* ctx, double* out_rate) {
  return wasapi_capture_get_pending_rate_change((wasapi_capture_t*)ctx,
                                                out_rate);
}
static bool cap_vtable_is_pitch_control_supported(void* ctx) {
  return wasapi_capture_pitch_control_supported((wasapi_capture_t*)ctx);
}
static void cap_vtable_set_pitch(void* ctx, double multiplier) {
  wasapi_capture_set_pitch((wasapi_capture_t*)ctx, multiplier);
}
static bool cap_vtable_wait_for_data(void* ctx, uint32_t timeout_ms) {
  return wasapi_capture_wait((wasapi_capture_t*)ctx, timeout_ms);
}
static void cap_vtable_destroy(void* ctx) {
  wasapi_capture_destroy((wasapi_capture_t*)ctx);
}

static const capture_backend_vtable_t wasapi_capture_vtable = {
    .open = cap_vtable_open,
    .read = cap_vtable_read,
    .close = cap_vtable_close,
    .get_pending_rate_change = cap_vtable_get_pending_rate_change,
    .is_pitch_control_supported = cap_vtable_is_pitch_control_supported,
    .set_pitch = cap_vtable_set_pitch,
    .wait_for_data = cap_vtable_wait_for_data,
    .destroy = cap_vtable_destroy};

capture_backend_t* wasapi_capture_create(const capture_device_config_t* config,
                                         int sample_rate, int chunk_size,
                                         processing_parameters_t* params,
                                         backend_error_t* err) {
  (void)params;
  (void)err;
  wasapi_capture_t* capture =
      (wasapi_capture_t*)calloc(1, sizeof(wasapi_capture_t));
  if (!capture) return NULL;

  if (config->has_device && strcmp(config->device, "default") != 0) {
    snprintf(capture->device, sizeof(capture->device), "%s", config->device);
  } else {
    capture->device[0] = '\0';
  }

  capture->sample_rate = sample_rate;
  capture->channels = config->channels;
  capture->chunk_size = chunk_size;
  capture->format = config->format;
  capture->loopback = config->loopback;

  capture_backend_t* backend =
      (capture_backend_t*)calloc(1, sizeof(capture_backend_t));
  if (!backend) {
    free(capture);
    return NULL;
  }
  backend->ctx = capture;
  backend->vtable = &wasapi_capture_vtable;
  return backend;
}

bool wasapi_capture_open(wasapi_capture_t* capture, backend_error_t* err) {
  CoInitializeEx(NULL, COINIT_MULTITHREADED);

  HRESULT hr =
      CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                       &IID_IMMDeviceEnumerator, (void**)&capture->enumerator);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create MMDeviceEnumerator");
    return false;
  }

  if (capture->device[0] == '\0') {
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
        capture->enumerator, capture->loopback ? eRender : eCapture, eConsole,
        &capture->mm_device);
  } else {
    IMMDeviceCollection* collection = NULL;
    hr = IMMDeviceEnumerator_EnumAudioEndpoints(
        capture->enumerator, capture->loopback ? eRender : eCapture,
        DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr)) {
      UINT count = 0;
      IMMDeviceCollection_GetCount(collection, &count);
      for (UINT i = 0; i < count; i++) {
        IMMDevice* dev = NULL;
        IMMDeviceCollection_Item(collection, i, &dev);
        bool matched = false;

        // 1. Try friendly name matching first
        IPropertyStore* properties = NULL;
        HRESULT hr_prop =
            IMMDevice_OpenPropertyStore(dev, STGM_READ, &properties);
        if (SUCCEEDED(hr_prop)) {
          PROPVARIANT var;
          PropVariantInit(&var);
          hr_prop = IPropertyStore_GetValue(properties,
                                            &PKEY_Device_FriendlyName, &var);
          if (SUCCEEDED(hr_prop) && var.vt == VT_LPWSTR) {
            char friendly_name[256] = {0};
            wcstombs(friendly_name, var.pwszVal, sizeof(friendly_name));
            if (strstr(friendly_name, capture->device) != NULL) {
              matched = true;
            }
            PropVariantClear(&var);
          }
          SAFE_RELEASE(properties);
        }

        // 2. Fallback to ID matching
        if (!matched) {
          LPWSTR id = NULL;
          IMMDevice_GetId(dev, &id);
          if (id) {
            char dev_id_char[256];
            wcstombs(dev_id_char, id, sizeof(dev_id_char));
            if (strstr(dev_id_char, capture->device) != NULL) {
              matched = true;
            }
            CoTaskMemFree(id);
          }
        }

        if (matched) {
          capture->mm_device = dev;
          break;
        }
        IMMDevice_Release(dev);
      }
      IMMDeviceCollection_Release(collection);
    }
  }

  if (!capture->mm_device) {
    SAFE_RELEASE(capture->enumerator);
    if (err)
      backend_error_init(err, BACKEND_ERROR_DEVICE_NOT_FOUND,
                         "WASAPI capture device not found");
    return false;
  }

  hr = IMMDevice_Activate(capture->mm_device, &IID_IAudioClient, CLSCTX_ALL,
                          NULL, (void**)&capture->client);
  if (FAILED(hr)) {
    SAFE_RELEASE(capture->mm_device);
    SAFE_RELEASE(capture->enumerator);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to activate IAudioClient");
    return false;
  }

  WAVEFORMATEXTENSIBLE wfx;
  memset(&wfx, 0, sizeof(wfx));
  wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wfx.Format.nChannels = capture->channels;
  wfx.Format.nSamplesPerSec = capture->sample_rate;
  wfx.Format.wBitsPerSample =
      (capture->format == WASAPI_SAMPLE_FORMAT_S16) ? 16 : 32;
  wfx.Format.nBlockAlign = (wfx.Format.wBitsPerSample / 8) * capture->channels;
  wfx.Format.nAvgBytesPerSec =
      wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
  wfx.Format.cbSize = 22;
  wfx.Samples.wValidBitsPerSample =
      (capture->format == WASAPI_SAMPLE_FORMAT_S24) ? 24
                                                    : wfx.Format.wBitsPerSample;
  wfx.dwChannelMask =
      (capture->channels == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
  wfx.SubFormat = (capture->format == WASAPI_SAMPLE_FORMAT_F32)
                      ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                      : KSDATAFORMAT_SUBTYPE_PCM;

  REFERENCE_TIME duration = 10000000;  // 1 second buffer
  DWORD flags = capture->loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;

  hr = IAudioClient_Initialize(capture->client, AUDCLNT_SHAREMODE_SHARED, flags,
                               duration, 0, (WAVEFORMATEX*)&wfx, NULL);
  if (FAILED(hr)) {
    SAFE_RELEASE(capture->client);
    SAFE_RELEASE(capture->mm_device);
    SAFE_RELEASE(capture->enumerator);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to initialize IAudioClient");
    return false;
  }

  hr =
      IAudioClient_GetBufferSize(capture->client, &capture->buffer_frame_count);
  hr = IAudioClient_GetService(capture->client, &IID_IAudioCaptureClient,
                               (void**)&capture->capture_client);
  if (FAILED(hr)) {
    SAFE_RELEASE(capture->client);
    SAFE_RELEASE(capture->mm_device);
    SAFE_RELEASE(capture->enumerator);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to get IAudioCaptureClient");
    return false;
  }

  IAudioClient_Start(capture->client);

  logger_t logger = logger_create("dsp.backend.wasapi");
  logger_info(
      &logger,
      "Opened WASAPI capture: device=%s, rate=%d, channels=%d, loopback=%d",
      log_arg_string(capture->device[0] != '\0' ? capture->device : "default"),
      log_arg_int((int64_t)capture->sample_rate),
      log_arg_int((int64_t)capture->channels),
      log_arg_int((int64_t)capture->loopback));

  return true;
}

bool wasapi_capture_read(wasapi_capture_t* capture, size_t frames,
                         audio_chunk_t* chunk, backend_error_t* err) {
  size_t frames_read = 0;

  while (frames_read < frames) {
    UINT32 packet_size = 0;
    HRESULT hr = IAudioCaptureClient_GetNextPacketSize(capture->capture_client,
                                                       &packet_size);
    if (FAILED(hr)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                           "Failed to get packet size");
      return false;
    }

    if (packet_size > 0) {
      BYTE* data = NULL;
      UINT32 num_frames = 0;
      DWORD flags = 0;
      hr = IAudioCaptureClient_GetBuffer(capture->capture_client, &data,
                                         &num_frames, &flags, NULL, NULL);
      if (SUCCEEDED(hr) && data) {
        UINT32 to_copy = frames - frames_read;
        if (to_copy > num_frames) to_copy = num_frames;

        decode_samples_from_wasapi(chunk, frames_read, data, to_copy,
                                   capture->format, capture->channels, flags);
        IAudioCaptureClient_ReleaseBuffer(capture->capture_client, num_frames);
        frames_read += to_copy;
      }
    } else {
      Sleep(1);
    }
  }

  chunk->valid_frames = frames;
  return true;
}

void wasapi_capture_close(wasapi_capture_t* capture) {
  if (capture->client) {
    IAudioClient_Stop(capture->client);
    SAFE_RELEASE(capture->capture_client);
    SAFE_RELEASE(capture->client);
  }
  SAFE_RELEASE(capture->mm_device);
  SAFE_RELEASE(capture->enumerator);
}

bool wasapi_capture_get_pending_rate_change(wasapi_capture_t* capture,
                                            double* out_rate) {
  (void)capture;
  (void)out_rate;
  return false;
}

bool wasapi_capture_pitch_control_supported(wasapi_capture_t* capture) {
  (void)capture;
  return false;
}

void wasapi_capture_set_pitch(wasapi_capture_t* capture, double multiplier) {
  (void)capture;
  (void)multiplier;
}

bool wasapi_capture_wait(wasapi_capture_t* capture, uint32_t timeout_ms) {
  if (!capture->capture_client) return false;
  UINT32 elapsed = 0;
  while (elapsed < timeout_ms) {
    UINT32 packet_size = 0;
    IAudioCaptureClient_GetNextPacketSize(capture->capture_client,
                                          &packet_size);
    if (packet_size > 0) return true;
    Sleep(1);
    elapsed++;
  }
  return false;
}

void wasapi_capture_destroy(wasapi_capture_t* capture) { free(capture); }

// MARK: - Playback Backend implementation

static bool play_vtable_open(void* ctx, backend_error_t* err) {
  return wasapi_playback_open((wasapi_playback_t*)ctx, err);
}
static bool play_vtable_write(void* ctx, const audio_chunk_t* chunk,
                              backend_error_t* err) {
  return wasapi_playback_write((wasapi_playback_t*)ctx, chunk, err);
}
static void play_vtable_close(void* ctx) {
  wasapi_playback_close((wasapi_playback_t*)ctx);
}
static size_t play_vtable_get_buffer_level(void* ctx) {
  return wasapi_playback_get_buffer_level((wasapi_playback_t*)ctx);
}
static bool play_vtable_get_pending_rate_change(void* ctx, double* out_rate) {
  return wasapi_playback_get_pending_rate_change((wasapi_playback_t*)ctx,
                                                 out_rate);
}
static bool play_vtable_prefill_silence(void* ctx, size_t frames,
                                        backend_error_t* err) {
  return wasapi_playback_prefill_silence((wasapi_playback_t*)ctx, frames, err);
}
static bool play_vtable_get_is_paused(void* ctx) {
  return wasapi_playback_get_is_paused((wasapi_playback_t*)ctx);
}
static void play_vtable_set_is_paused(void* ctx, bool paused) {
  wasapi_playback_set_is_paused((wasapi_playback_t*)ctx, paused);
}
static void play_vtable_destroy(void* ctx) {
  wasapi_playback_destroy((wasapi_playback_t*)ctx);
}

static const playback_backend_vtable_t wasapi_playback_vtable = {
    .open = play_vtable_open,
    .write = play_vtable_write,
    .close = play_vtable_close,
    .get_buffer_level = play_vtable_get_buffer_level,
    .get_pending_rate_change = play_vtable_get_pending_rate_change,
    .prefill_silence = play_vtable_prefill_silence,
    .get_is_paused = play_vtable_get_is_paused,
    .set_is_paused = play_vtable_set_is_paused,
    .destroy = play_vtable_destroy};

playback_backend_t* wasapi_playback_create(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    processing_parameters_t* params, backend_error_t* err) {
  (void)params;
  (void)err;
  wasapi_playback_t* playback =
      (wasapi_playback_t*)calloc(1, sizeof(wasapi_playback_t));
  if (!playback) return NULL;

  if (config->has_device && strcmp(config->device, "default") != 0) {
    snprintf(playback->device, sizeof(playback->device), "%s", config->device);
  } else {
    playback->device[0] = '\0';
  }

  playback->sample_rate = sample_rate;
  playback->channels = config->channels;
  playback->chunk_size = chunk_size;
  playback->format = config->format;
  playback->exclusive = config->exclusive;

  playback_backend_t* backend =
      (playback_backend_t*)calloc(1, sizeof(playback_backend_t));
  if (!backend) {
    free(playback);
    return NULL;
  }
  backend->ctx = playback;
  backend->vtable = &wasapi_playback_vtable;
  return backend;
}

bool wasapi_playback_open(wasapi_playback_t* playback, backend_error_t* err) {
  CoInitializeEx(NULL, COINIT_MULTITHREADED);

  HRESULT hr =
      CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                       &IID_IMMDeviceEnumerator, (void**)&playback->enumerator);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create MMDeviceEnumerator");
    return false;
  }

  if (playback->device[0] == '\0') {
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
        playback->enumerator, eRender, eConsole, &playback->mm_device);
  } else {
    IMMDeviceCollection* collection = NULL;
    hr = IMMDeviceEnumerator_EnumAudioEndpoints(
        playback->enumerator, eRender, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr)) {
      UINT count = 0;
      IMMDeviceCollection_GetCount(collection, &count);
      for (UINT i = 0; i < count; i++) {
        IMMDevice* dev = NULL;
        IMMDeviceCollection_Item(collection, i, &dev);
        bool matched = false;

        // 1. Try friendly name matching first
        IPropertyStore* properties = NULL;
        HRESULT hr_prop =
            IMMDevice_OpenPropertyStore(dev, STGM_READ, &properties);
        if (SUCCEEDED(hr_prop)) {
          PROPVARIANT var;
          PropVariantInit(&var);
          hr_prop = IPropertyStore_GetValue(properties,
                                            &PKEY_Device_FriendlyName, &var);
          if (SUCCEEDED(hr_prop) && var.vt == VT_LPWSTR) {
            char friendly_name[256] = {0};
            wcstombs(friendly_name, var.pwszVal, sizeof(friendly_name));
            if (strstr(friendly_name, playback->device) != NULL) {
              matched = true;
            }
            PropVariantClear(&var);
          }
          SAFE_RELEASE(properties);
        }

        // 2. Fallback to ID matching
        if (!matched) {
          LPWSTR id = NULL;
          IMMDevice_GetId(dev, &id);
          if (id) {
            char dev_id_char[256];
            wcstombs(dev_id_char, id, sizeof(dev_id_char));
            if (strstr(dev_id_char, playback->device) != NULL) {
              matched = true;
            }
            CoTaskMemFree(id);
          }
        }

        if (matched) {
          playback->mm_device = dev;
          break;
        }
        IMMDevice_Release(dev);
      }
      IMMDeviceCollection_Release(collection);
    }
  }

  if (!playback->mm_device) {
    SAFE_RELEASE(playback->enumerator);
    if (err)
      backend_error_init(err, BACKEND_ERROR_DEVICE_NOT_FOUND,
                         "WASAPI playback device not found");
    return false;
  }

  hr = IMMDevice_Activate(playback->mm_device, &IID_IAudioClient, CLSCTX_ALL,
                          NULL, (void**)&playback->client);
  if (FAILED(hr)) {
    SAFE_RELEASE(playback->mm_device);
    SAFE_RELEASE(playback->enumerator);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to activate IAudioClient");
    return false;
  }

  WAVEFORMATEXTENSIBLE wfx;
  memset(&wfx, 0, sizeof(wfx));
  wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wfx.Format.nChannels = playback->channels;
  wfx.Format.nSamplesPerSec = playback->sample_rate;
  wfx.Format.wBitsPerSample =
      (playback->format == WASAPI_SAMPLE_FORMAT_S16) ? 16 : 32;
  wfx.Format.nBlockAlign = (wfx.Format.wBitsPerSample / 8) * playback->channels;
  wfx.Format.nAvgBytesPerSec =
      wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
  wfx.Format.cbSize = 22;
  wfx.Samples.wValidBitsPerSample =
      (playback->format == WASAPI_SAMPLE_FORMAT_S24)
          ? 24
          : wfx.Format.wBitsPerSample;
  wfx.dwChannelMask = (playback->channels == 2)
                          ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                          : 0;
  wfx.SubFormat = (playback->format == WASAPI_SAMPLE_FORMAT_F32)
                      ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                      : KSDATAFORMAT_SUBTYPE_PCM;

  REFERENCE_TIME duration = 10000000;
  AUDCLNT_SHAREMODE mode = playback->exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE
                                               : AUDCLNT_SHAREMODE_SHARED;

  hr = IAudioClient_Initialize(playback->client, mode, 0, duration,
                               playback->exclusive ? duration : 0,
                               (WAVEFORMATEX*)&wfx, NULL);
  if (FAILED(hr)) {
    SAFE_RELEASE(playback->client);
    SAFE_RELEASE(playback->mm_device);
    SAFE_RELEASE(playback->enumerator);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to initialize IAudioClient");
    return false;
  }

  hr = IAudioClient_GetBufferSize(playback->client,
                                  &playback->buffer_frame_count);
  hr = IAudioClient_GetService(playback->client, &IID_IAudioRenderClient,
                               (void**)&playback->render_client);
  if (FAILED(hr)) {
    SAFE_RELEASE(playback->client);
    SAFE_RELEASE(playback->mm_device);
    SAFE_RELEASE(playback->enumerator);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to get IAudioRenderClient");
    return false;
  }

  playback->paused = false;
  IAudioClient_Start(playback->client);

  logger_t logger = logger_create("dsp.backend.wasapi");
  logger_info(
      &logger,
      "Opened WASAPI playback: device=%s, rate=%d, channels=%d, exclusive=%d",
      log_arg_string(playback->device[0] != '\0' ? playback->device
                                                 : "default"),
      log_arg_int((int64_t)playback->sample_rate),
      log_arg_int((int64_t)playback->channels),
      log_arg_int((int64_t)playback->exclusive));

  return true;
}

bool wasapi_playback_write(wasapi_playback_t* playback,
                           const audio_chunk_t* chunk, backend_error_t* err) {
  if (playback->paused) return true;

  size_t frames_written = 0;
  size_t total_frames = chunk->valid_frames;

  while (frames_written < total_frames) {
    UINT32 padding = 0;
    HRESULT hr = IAudioClient_GetCurrentPadding(playback->client, &padding);
    if (FAILED(hr)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "Failed to get padding");
      return false;
    }

    UINT32 available_frames = playback->buffer_frame_count - padding;
    UINT32 to_write = total_frames - frames_written;
    if (to_write > available_frames) {
      to_write = available_frames;
    }

    if (to_write > 0) {
      BYTE* data = NULL;
      hr = IAudioRenderClient_GetBuffer(playback->render_client, to_write,
                                        &data);
      if (SUCCEEDED(hr) && data) {
        encode_samples_to_wasapi(data, chunk, frames_written, to_write,
                                 playback->format, playback->channels);
        IAudioRenderClient_ReleaseBuffer(playback->render_client, to_write, 0);
        frames_written += to_write;
      }
    } else {
      Sleep(1);
    }
  }
  return true;
}

void wasapi_playback_close(wasapi_playback_t* playback) {
  if (playback->client) {
    IAudioClient_Stop(playback->client);
    SAFE_RELEASE(playback->render_client);
    SAFE_RELEASE(playback->client);
  }
  SAFE_RELEASE(playback->mm_device);
  SAFE_RELEASE(playback->enumerator);
}

size_t wasapi_playback_get_buffer_level(wasapi_playback_t* playback) {
  if (!playback->client) return 0;
  UINT32 padding = 0;
  IAudioClient_GetCurrentPadding(playback->client, &padding);
  return padding;
}

bool wasapi_playback_get_pending_rate_change(wasapi_playback_t* playback,
                                             double* out_rate) {
  (void)playback;
  (void)out_rate;
  return false;
}

bool wasapi_playback_prefill_silence(wasapi_playback_t* playback, size_t frames,
                                     backend_error_t* err) {
  if (!playback->render_client) return false;
  BYTE* data = NULL;
  HRESULT hr = IAudioRenderClient_GetBuffer(playback->render_client,
                                            (UINT32)frames, &data);
  if (SUCCEEDED(hr) && data) {
    memset(data, 0,
           frames * playback->channels * get_sample_size(playback->format));
    IAudioRenderClient_ReleaseBuffer(playback->render_client, (UINT32)frames,
                                     0);
    return true;
  }
  if (err)
    backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                       "Failed to prefill silence");
  return false;
}

bool wasapi_playback_get_is_paused(wasapi_playback_t* playback) {
  return playback->paused;
}

void wasapi_playback_set_is_paused(wasapi_playback_t* playback, bool paused) {
  playback->paused = paused;
}

void wasapi_playback_destroy(wasapi_playback_t* playback) { free(playback); }

#endif  // _WIN32
