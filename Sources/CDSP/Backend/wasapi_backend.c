#if defined(ENABLE_WASAPI)

#define WIN32_LEAN_AND_MEAN
#include "wasapi_backend.h"

#include <initguid.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
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
  bool exclusive;
  bool polling;

  int bits_per_sample;
  int valid_bits;
  bool is_float;
  bool com_initialized;

  IMMDeviceEnumerator* enumerator;
  IMMDevice* mm_device;
  IAudioClient* client;
  IAudioCaptureClient* capture_client;
  UINT32 buffer_frame_count;
  HANDLE event;
};

struct wasapi_playback {
  char device[256];
  int sample_rate;
  int channels;
  int chunk_size;
  wasapi_sample_format_t format;
  bool exclusive;
  bool polling;

  int bits_per_sample;
  int valid_bits;
  bool is_float;
  bool com_initialized;

  IMMDeviceEnumerator* enumerator;
  IMMDevice* mm_device;
  IAudioClient* client;
  IAudioRenderClient* render_client;
  UINT32 buffer_frame_count;
  bool paused;
  HANDLE event;
};

static inline void decode_samples_from_wasapi(audio_chunk_t* chunk,
                                              size_t chunk_offset,
                                              const BYTE* src, size_t frames,
                                              int channels, DWORD flags,
                                              int bits_per_sample,
                                              int valid_bits,
                                              bool is_float) {
  if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        audio_chunk_get_channel(chunk, c)[chunk_offset + f] = 0.0;
      }
    }
    return;
  }

  if (is_float) {
    const float* f32 = (const float*)src;
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        audio_chunk_get_channel(chunk, c)[chunk_offset + f] =
            (double)f32[f * channels + c];
      }
    }
    return;
  }

  if (bits_per_sample == 16) {
    const int16_t* s16 = (const int16_t*)src;
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        audio_chunk_get_channel(chunk, c)[chunk_offset + f] =
            (double)s16[f * channels + c] / 32768.0;
      }
    }
  } else if (bits_per_sample == 24) {
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        size_t idx = (f * channels + c) * 3;
        int32_t val = (src[idx] | (src[idx + 1] << 8) | (src[idx + 2] << 16));
        if (val & 0x800000) {
          val |= 0xFF000000;
        }
        audio_chunk_get_channel(chunk, c)[chunk_offset + f] =
            (double)val / 8388608.0;
      }
    }
  } else if (bits_per_sample == 32 && valid_bits == 24) {
    const int32_t* s32 = (const int32_t*)src;
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        int32_t val = s32[f * channels + c] >> 8;
        audio_chunk_get_channel(chunk, c)[chunk_offset + f] =
            (double)val / 8388608.0;
      }
    }
  } else if (bits_per_sample == 32 && valid_bits == 32) {
    const int32_t* s32 = (const int32_t*)src;
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        audio_chunk_get_channel(chunk, c)[chunk_offset + f] =
            (double)s32[f * channels + c] / 2147483648.0;
      }
    }
  }
}

static inline void encode_samples_to_wasapi(BYTE* dst,
                                            const audio_chunk_t* chunk,
                                            size_t chunk_offset, size_t frames,
                                            int channels,
                                            int bits_per_sample,
                                            int valid_bits,
                                            bool is_float) {
  if (is_float) {
    float* f32 = (float*)dst;
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        f32[f * channels + c] =
            (float)audio_chunk_get_channel(chunk, c)[chunk_offset + f];
      }
    }
    return;
  }

  if (bits_per_sample == 16) {
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
  } else if (bits_per_sample == 24) {
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[chunk_offset + f];
        if (val > 1.0)
          val = 1.0;
        else if (val < -1.0)
          val = -1.0;
        int32_t val24 = (int32_t)(val * 8388607.0);
        size_t idx = (f * channels + c) * 3;
        dst[idx] = val24 & 0xFF;
        dst[idx + 1] = (val24 >> 8) & 0xFF;
        dst[idx + 2] = (val24 >> 16) & 0xFF;
      }
    }
  } else if (bits_per_sample == 32 && valid_bits == 24) {
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
  } else if (bits_per_sample == 32 && valid_bits == 32) {
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
  capture->exclusive = config->exclusive;
  capture->polling = config->has_polling ? config->polling : false;

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
  HRESULT init_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  capture->com_initialized = SUCCEEDED(init_hr);

  HRESULT hr =
      CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                       &IID_IMMDeviceEnumerator, (void**)&capture->enumerator);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create MMDeviceEnumerator");
    goto error_cleanup;
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
    if (err)
      backend_error_init(err, BACKEND_ERROR_DEVICE_NOT_FOUND,
                         "WASAPI capture device not found");
    goto error_cleanup;
  }

  hr = IMMDevice_Activate(capture->mm_device, &IID_IAudioClient, CLSCTX_ALL,
                          NULL, (void**)&capture->client);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to activate IAudioClient");
    goto error_cleanup;
  }

  AUDCLNT_SHAREMODE mode = AUDCLNT_SHAREMODE_SHARED;
  if (capture->exclusive && !capture->loopback) {
    mode = AUDCLNT_SHAREMODE_EXCLUSIVE;
  }

  WAVEFORMATEXTENSIBLE wfx;
  memset(&wfx, 0, sizeof(wfx));
  wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wfx.Format.nChannels = capture->channels;
  wfx.Format.nSamplesPerSec = capture->sample_rate;
  wfx.Format.cbSize = 22;
  wfx.dwChannelMask =
      (capture->channels == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;

  bool format_found = false;
  if (mode == AUDCLNT_SHAREMODE_SHARED) {
    wfx.Format.wBitsPerSample = 32;
    wfx.Format.nBlockAlign = 4 * capture->channels;
    wfx.Format.nAvgBytesPerSec = capture->sample_rate * wfx.Format.nBlockAlign;
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    capture->bits_per_sample = 32;
    capture->valid_bits = 32;
    capture->is_float = true;
    format_found = true;
  } else {
    if (capture->format == WASAPI_SAMPLE_FORMAT_S16) {
      wfx.Format.wBitsPerSample = 16;
      wfx.Format.nBlockAlign = 2 * capture->channels;
      wfx.Format.nAvgBytesPerSec = capture->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 16;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      hr = IAudioClient_IsFormatSupported(capture->client, mode, (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
        capture->bits_per_sample = 16;
        capture->valid_bits = 16;
        capture->is_float = false;
        format_found = true;
      }
    } else if (capture->format == WASAPI_SAMPLE_FORMAT_S32) {
      wfx.Format.wBitsPerSample = 32;
      wfx.Format.nBlockAlign = 4 * capture->channels;
      wfx.Format.nAvgBytesPerSec = capture->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 32;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      hr = IAudioClient_IsFormatSupported(capture->client, mode, (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
        capture->bits_per_sample = 32;
        capture->valid_bits = 32;
        capture->is_float = false;
        format_found = true;
      }
    } else if (capture->format == WASAPI_SAMPLE_FORMAT_F32) {
      wfx.Format.wBitsPerSample = 32;
      wfx.Format.nBlockAlign = 4 * capture->channels;
      wfx.Format.nAvgBytesPerSec = capture->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 32;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
      hr = IAudioClient_IsFormatSupported(capture->client, mode, (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
        capture->bits_per_sample = 32;
        capture->valid_bits = 32;
        capture->is_float = true;
        format_found = true;
      }
    } else if (capture->format == WASAPI_SAMPLE_FORMAT_S24) {
      wfx.Format.wBitsPerSample = 24;
      wfx.Format.nBlockAlign = 3 * capture->channels;
      wfx.Format.nAvgBytesPerSec = capture->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 24;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      hr = IAudioClient_IsFormatSupported(capture->client, mode, (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
        capture->bits_per_sample = 24;
        capture->valid_bits = 24;
        capture->is_float = false;
        format_found = true;
      } else {
        wfx.Format.wBitsPerSample = 32;
        wfx.Format.nBlockAlign = 4 * capture->channels;
        wfx.Format.nAvgBytesPerSec = capture->sample_rate * wfx.Format.nBlockAlign;
        wfx.Samples.wValidBitsPerSample = 24;
        wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        hr = IAudioClient_IsFormatSupported(capture->client, mode, (WAVEFORMATEX*)&wfx, NULL);
        if (SUCCEEDED(hr)) {
          capture->bits_per_sample = 32;
          capture->valid_bits = 24;
          capture->is_float = false;
          format_found = true;
        }
      }
    }
  }

  if (!format_found) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Unsupported sample format");
    goto error_cleanup;
  }

  REFERENCE_TIME duration =
      (REFERENCE_TIME)(((double)capture->chunk_size / capture->sample_rate) *
                       10000000.0);
  if (mode == AUDCLNT_SHAREMODE_SHARED) {
    duration = 10000000;
  }
  DWORD flags = (capture->loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
  if (!capture->polling) {
    flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
  }

  hr = IAudioClient_Initialize(
      capture->client, mode, flags, duration,
      (mode == AUDCLNT_SHAREMODE_EXCLUSIVE) ? duration : 0, (WAVEFORMATEX*)&wfx,
      NULL);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to initialize IAudioClient");
    goto error_cleanup;
  }

  if (!capture->polling) {
    capture->event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!capture->event) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to create event handle");
      goto error_cleanup;
    }

    hr = IAudioClient_SetEventHandle(capture->client, capture->event);
    if (FAILED(hr)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to set event handle");
      goto error_cleanup;
    }
  } else {
    capture->event = NULL;
  }

  hr = IAudioClient_GetBufferSize(capture->client, &capture->buffer_frame_count);
  hr = IAudioClient_GetService(capture->client, &IID_IAudioCaptureClient,
                               (void**)&capture->capture_client);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to get IAudioCaptureClient");
    goto error_cleanup;
  }

  IAudioClient_Start(capture->client);

  logger_t logger = logger_create("dsp.backend.wasapi");
  logger_info(
      &logger, "Opened WASAPI capture: device=%s, rate=%d, channels=%d",
      log_arg_string(capture->device[0] != '\0' ? capture->device : "default"),
      log_arg_int((int64_t)capture->sample_rate),
      log_arg_int((int64_t)capture->channels), log_arg_none());
  logger_info(&logger, "WASAPI capture options: loopback=%d, exclusive=%d",
              log_arg_int((int64_t)capture->loopback),
              log_arg_int((int64_t)capture->exclusive), log_arg_none(),
              log_arg_none());

  return true;

error_cleanup:
  if (capture->capture_client) {
    SAFE_RELEASE(capture->capture_client);
  }
  if (capture->client) {
    SAFE_RELEASE(capture->client);
  }
  if (capture->mm_device) {
    SAFE_RELEASE(capture->mm_device);
  }
  if (capture->enumerator) {
    SAFE_RELEASE(capture->enumerator);
  }
  if (capture->event) {
    CloseHandle(capture->event);
    capture->event = NULL;
  }
  if (capture->com_initialized) {
    CoUninitialize();
    capture->com_initialized = false;
  }
  return false;
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
                                   capture->channels, flags,
                                   capture->bits_per_sample,
                                   capture->valid_bits,
                                   capture->is_float);
        IAudioCaptureClient_ReleaseBuffer(capture->capture_client, num_frames);
        frames_read += to_copy;
      }
    } else {
      if (capture->polling) {
        Sleep(1);
      } else {
        if (WaitForSingleObject(capture->event, 100) != WAIT_OBJECT_0) {
          // Timeout or error wait
        }
      }
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
  if (capture->event) {
    CloseHandle(capture->event);
    capture->event = NULL;
  }
  SAFE_RELEASE(capture->mm_device);
  SAFE_RELEASE(capture->enumerator);

  if (capture->com_initialized) {
    CoUninitialize();
    capture->com_initialized = false;
  }
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
  if (capture->polling) {
    Sleep(1);
    return true;
  }
  if (!capture->event) return false;
  return WaitForSingleObject(capture->event, timeout_ms) == WAIT_OBJECT_0;
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
  playback->polling = config->has_polling ? config->polling : false;

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
  HRESULT init_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  playback->com_initialized = SUCCEEDED(init_hr);

  HRESULT hr =
      CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                       &IID_IMMDeviceEnumerator, (void**)&playback->enumerator);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create MMDeviceEnumerator");
    goto error_cleanup;
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
    if (err)
      backend_error_init(err, BACKEND_ERROR_DEVICE_NOT_FOUND,
                         "WASAPI playback device not found");
    goto error_cleanup;
  }

  hr = IMMDevice_Activate(playback->mm_device, &IID_IAudioClient, CLSCTX_ALL,
                          NULL, (void**)&playback->client);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to activate IAudioClient");
    goto error_cleanup;
  }

  AUDCLNT_SHAREMODE mode = playback->exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE
                                               : AUDCLNT_SHAREMODE_SHARED;

  WAVEFORMATEXTENSIBLE wfx;
  memset(&wfx, 0, sizeof(wfx));
  wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wfx.Format.nChannels = playback->channels;
  wfx.Format.nSamplesPerSec = playback->sample_rate;
  wfx.Format.cbSize = 22;
  wfx.dwChannelMask = (playback->channels == 2)
                          ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                          : 0;

  bool format_found = false;
  if (mode == AUDCLNT_SHAREMODE_SHARED) {
    wfx.Format.wBitsPerSample = 32;
    wfx.Format.nBlockAlign = 4 * playback->channels;
    wfx.Format.nAvgBytesPerSec = playback->sample_rate * wfx.Format.nBlockAlign;
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    playback->bits_per_sample = 32;
    playback->valid_bits = 32;
    playback->is_float = true;
    format_found = true;
  } else {
    if (playback->format == WASAPI_SAMPLE_FORMAT_S16) {
      wfx.Format.wBitsPerSample = 16;
      wfx.Format.nBlockAlign = 2 * playback->channels;
      wfx.Format.nAvgBytesPerSec = playback->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 16;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      hr = IAudioClient_IsFormatSupported(playback->client, mode, (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
        playback->bits_per_sample = 16;
        playback->valid_bits = 16;
        playback->is_float = false;
        format_found = true;
      }
    } else if (playback->format == WASAPI_SAMPLE_FORMAT_S32) {
      wfx.Format.wBitsPerSample = 32;
      wfx.Format.nBlockAlign = 4 * playback->channels;
      wfx.Format.nAvgBytesPerSec = playback->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 32;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      hr = IAudioClient_IsFormatSupported(playback->client, mode, (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
        playback->bits_per_sample = 32;
        playback->valid_bits = 32;
        playback->is_float = false;
        format_found = true;
      }
    } else if (playback->format == WASAPI_SAMPLE_FORMAT_F32) {
      wfx.Format.wBitsPerSample = 32;
      wfx.Format.nBlockAlign = 4 * playback->channels;
      wfx.Format.nAvgBytesPerSec = playback->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 32;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
      hr = IAudioClient_IsFormatSupported(playback->client, mode, (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
        playback->bits_per_sample = 32;
        playback->valid_bits = 32;
        playback->is_float = true;
        format_found = true;
      }
    } else if (playback->format == WASAPI_SAMPLE_FORMAT_S24) {
      wfx.Format.wBitsPerSample = 24;
      wfx.Format.nBlockAlign = 3 * playback->channels;
      wfx.Format.nAvgBytesPerSec = playback->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 24;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      hr = IAudioClient_IsFormatSupported(playback->client, mode, (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
        playback->bits_per_sample = 24;
        playback->valid_bits = 24;
        playback->is_float = false;
        format_found = true;
      } else {
        wfx.Format.wBitsPerSample = 32;
        wfx.Format.nBlockAlign = 4 * playback->channels;
        wfx.Format.nAvgBytesPerSec = playback->sample_rate * wfx.Format.nBlockAlign;
        wfx.Samples.wValidBitsPerSample = 24;
        wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        hr = IAudioClient_IsFormatSupported(playback->client, mode, (WAVEFORMATEX*)&wfx, NULL);
        if (SUCCEEDED(hr)) {
          playback->bits_per_sample = 32;
          playback->valid_bits = 24;
          playback->is_float = false;
          format_found = true;
        }
      }
    }
  }

  if (!format_found) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Unsupported sample format");
    goto error_cleanup;
  }

  REFERENCE_TIME duration = 10000000;
  DWORD flags = 0;
  if (!playback->polling) {
    flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
  }

  hr = IAudioClient_Initialize(
      playback->client, mode, flags, duration,
      playback->exclusive ? duration : 0, (WAVEFORMATEX*)&wfx, NULL);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to initialize IAudioClient");
    goto error_cleanup;
  }

  if (!playback->polling) {
    playback->event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!playback->event) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to create event handle");
      goto error_cleanup;
    }

    hr = IAudioClient_SetEventHandle(playback->client, playback->event);
    if (FAILED(hr)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to set event handle");
      goto error_cleanup;
    }
  } else {
    playback->event = NULL;
  }

  hr = IAudioClient_GetBufferSize(playback->client,
                                  &playback->buffer_frame_count);
  hr = IAudioClient_GetService(playback->client, &IID_IAudioRenderClient,
                               (void**)&playback->render_client);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to get IAudioRenderClient");
    goto error_cleanup;
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

error_cleanup:
  if (playback->render_client) {
    SAFE_RELEASE(playback->render_client);
  }
  if (playback->client) {
    SAFE_RELEASE(playback->client);
  }
  if (playback->mm_device) {
    SAFE_RELEASE(playback->mm_device);
  }
  if (playback->enumerator) {
    SAFE_RELEASE(playback->enumerator);
  }
  if (playback->event) {
    CloseHandle(playback->event);
    playback->event = NULL;
  }
  if (playback->com_initialized) {
    CoUninitialize();
    playback->com_initialized = false;
  }
  return false;
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
                                 playback->channels,
                                 playback->bits_per_sample,
                                 playback->valid_bits,
                                 playback->is_float);
        IAudioRenderClient_ReleaseBuffer(playback->render_client, to_write, 0);
        frames_written += to_write;
      }
    } else {
      if (playback->polling) {
        Sleep(1);
      } else {
        if (WaitForSingleObject(playback->event, 100) != WAIT_OBJECT_0) {
          // Timeout or error wait
        }
      }
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
  if (playback->event) {
    CloseHandle(playback->event);
    playback->event = NULL;
  }
  SAFE_RELEASE(playback->mm_device);
  SAFE_RELEASE(playback->enumerator);

  if (playback->com_initialized) {
    CoUninitialize();
    playback->com_initialized = false;
  }
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
           frames * playback->channels * (playback->bits_per_sample / 8));
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

#endif  // ENABLE_WASAPI
