#if defined(ENABLE_WASAPI)

#define WIN32_LEAN_AND_MEAN
// clang-format off
#include <initguid.h>
// clang-format on
#include "wasapi_backend.h"

#include "wasapi_capabilities.h"


#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "Logging/app_logger.h"

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

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
  audio_chunk_t* residual_chunk;
  size_t residual_frames;
  size_t residual_offset;
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
  _Atomic bool paused;
  HANDLE event;
};

/**
 * @brief Decodes interleaved audio samples from WASAPI input buffer format to
 * deinterleaved double format.
 *
 * Supports conversion from:
 * - 32-bit IEEE float
 * - 16-bit signed integer (scaled to [-1.0, 1.0])
 * - 24-bit signed integer (packed 3 bytes, scaled to [-1.0, 1.0])
 * - 32-bit signed integer with 24 valid bits (shifted and scaled)
 * - 32-bit signed integer with 32 valid bits (scaled)
 *
 * If AUDCLNT_BUFFERFLAGS_SILENT is set in flags, the output chunk is filled
 * with silence (0.0).
 *
 * @param chunk The destination audio_chunk_t structure.
 * @param chunk_offset Starting frame index in the destination chunk.
 * @param src Pointer to the source raw WASAPI byte buffer.
 * @param frames Number of frames to decode.
 * @param channels Number of audio channels.
 * @param flags Buffer status flags from WASAPI (e.g., silence flag).
 * @param bits_per_sample Total bits per sample in the source container (16, 24,
 * 32).
 * @param valid_bits Actual bits of precision (16, 24, 32).
 * @param is_float True if the source is floating-point, false for PCM.
 */
static inline void decode_samples_from_wasapi(audio_chunk_t* chunk,
                                              size_t chunk_offset,
                                              const BYTE* src, size_t frames,
                                              int channels, DWORD flags,
                                              int bits_per_sample,
                                              int valid_bits, bool is_float) {
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

/**
 * @brief Encodes deinterleaved double samples from audio chunk to interleaved
 * WASAPI output buffer format.
 *
 * Clamps input double values to [-1.0, 1.0] before encoding to integer formats.
 * Supports conversion to:
 * - 32-bit IEEE float
 * - 16-bit signed integer
 * - 24-bit signed integer (packed 3 bytes)
 * - 32-bit signed integer with 24 valid bits (shifted)
 * - 32-bit signed integer with 32 valid bits
 *
 * @param dst Pointer to the destination raw WASAPI byte buffer.
 * @param chunk The source audio_chunk_t structure.
 * @param chunk_offset Starting frame index in the source chunk.
 * @param frames Number of frames to encode.
 * @param channels Number of audio channels.
 * @param bits_per_sample Total bits per sample in the destination container
 * (16, 24, 32).
 * @param valid_bits Actual bits of precision (16, 24, 32).
 * @param is_float True if the destination format is floating-point.
 */
static inline void encode_samples_to_wasapi(BYTE* dst,
                                            const audio_chunk_t* chunk,
                                            size_t chunk_offset, size_t frames,
                                            int channels, int bits_per_sample,
                                            int valid_bits, bool is_float) {
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

/**
 * @brief Vtable adapter to open the WASAPI capture stream.
 * @param ctx Pointer to the wasapi_capture_t context.
 * @param err Pointer to backend_error_t to receive error details.
 * @return true if successful, false otherwise.
 */
static bool cap_vtable_open(void* ctx, backend_error_t* err) {
  return wasapi_capture_open((wasapi_capture_t*)ctx, err);
}

/**
 * @brief Vtable adapter to read frames from the WASAPI capture stream.
 * @param ctx Pointer to the wasapi_capture_t context.
 * @param frames Number of frames to read.
 * @param chunk Pointer to audio_chunk_t to store the read audio data.
 * @param err Pointer to backend_error_t to receive error details.
 * @return true if successful, false otherwise.
 */
static bool cap_vtable_read(void* ctx, size_t frames, audio_chunk_t* chunk,
                            backend_error_t* err) {
  return wasapi_capture_read((wasapi_capture_t*)ctx, frames, chunk, err);
}

/**
 * @brief Vtable adapter to close the WASAPI capture stream.
 * @param ctx Pointer to the wasapi_capture_t context.
 */
static void cap_vtable_close(void* ctx) {
  wasapi_capture_close((wasapi_capture_t*)ctx);
}

/**
 * @brief Vtable adapter to check for pending rate changes.
 * @note WASAPI backend does not support dynamic rate changes.
 * @param ctx Pointer to the wasapi_capture_t context.
 * @param out_rate Pointer to store the new rate (unused).
 * @return Always false.
 */
static bool cap_vtable_get_pending_rate_change(void* ctx, double* out_rate) {
  return wasapi_capture_get_pending_rate_change((wasapi_capture_t*)ctx,
                                                out_rate);
}

/**
 * @brief Vtable adapter to check if pitch control is supported.
 * @note WASAPI backend does not support pitch control.
 * @param ctx Pointer to the wasapi_capture_t context.
 * @return Always false.
 */
static bool cap_vtable_is_pitch_control_supported(void* ctx) {
  return wasapi_capture_pitch_control_supported((wasapi_capture_t*)ctx);
}

/**
 * @brief Vtable adapter to set pitch multiplier.
 * @note WASAPI backend does not support pitch control (no-op).
 * @param ctx Pointer to the wasapi_capture_t context.
 * @param multiplier The pitch multiplier.
 */
static void cap_vtable_set_pitch(void* ctx, double multiplier) {
  wasapi_capture_set_pitch((wasapi_capture_t*)ctx, multiplier);
}

/**
 * @brief Vtable adapter to wait for capture data to become available.
 * @param ctx Pointer to the wasapi_capture_t context.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if data is available, false on timeout.
 */
static bool cap_vtable_wait_for_data(void* ctx, uint32_t timeout_ms) {
  return wasapi_capture_wait((wasapi_capture_t*)ctx, timeout_ms);
}

/**
 * @brief Vtable adapter to destroy the WASAPI capture context.
 * @param ctx Pointer to the wasapi_capture_t context.
 */
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

  if (config->cfg.wasapi.has_device &&
      strcmp(config->cfg.wasapi.device, "default") != 0) {
    snprintf(capture->device, sizeof(capture->device), "%s",
             config->cfg.wasapi.device);
  } else {
    capture->device[0] = '\0';
  }

  capture->sample_rate = sample_rate;
  capture->channels = config->cfg.wasapi.channels;
  capture->chunk_size = chunk_size;
  capture->format = config->cfg.wasapi.format;
  capture->loopback = config->cfg.wasapi.loopback;
  capture->exclusive = config->cfg.wasapi.exclusive;
  capture->polling =
      config->cfg.wasapi.has_polling ? config->cfg.wasapi.polling : false;

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
  // Initialize COM library for multithreaded operations.
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

  // Retrieve IMMDevice. If no device name is specified, get default.
  // Otherwise, enumerate endpoints and match friendly name or ID.
  capture->mm_device = wasapi_find_device_by_name(
      capture->enumerator, capture->device, !capture->loopback);

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

  // Set up the wave format structure.
  // We use WAVEFORMATEXTENSIBLE which is required for formats with >2 channels
  // or high precision sample types (>16-bit).
  WAVEFORMATEXTENSIBLE wfx;
  memset(&wfx, 0, sizeof(wfx));
  wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wfx.Format.nChannels = capture->channels;
  wfx.Format.nSamplesPerSec = capture->sample_rate;
  wfx.Format.cbSize = 22;
  wfx.dwChannelMask =
      (capture->channels == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;

  // WASAPI Shared mode usually only supports IEEE Float formats.
  // Exclusive mode can support raw PCM.
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
      wfx.Format.nAvgBytesPerSec =
          capture->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 16;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      hr = IAudioClient_IsFormatSupported(capture->client, mode,
                                          (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
        capture->bits_per_sample = 16;
        capture->valid_bits = 16;
        capture->is_float = false;
        format_found = true;
      }
    } else if (capture->format == WASAPI_SAMPLE_FORMAT_S32) {
      wfx.Format.wBitsPerSample = 32;
      wfx.Format.nBlockAlign = 4 * capture->channels;
      wfx.Format.nAvgBytesPerSec =
          capture->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 32;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      hr = IAudioClient_IsFormatSupported(capture->client, mode,
                                          (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
        capture->bits_per_sample = 32;
        capture->valid_bits = 32;
        capture->is_float = false;
        format_found = true;
      }
    } else if (capture->format == WASAPI_SAMPLE_FORMAT_F32) {
      wfx.Format.wBitsPerSample = 32;
      wfx.Format.nBlockAlign = 4 * capture->channels;
      wfx.Format.nAvgBytesPerSec =
          capture->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 32;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
      hr = IAudioClient_IsFormatSupported(capture->client, mode,
                                          (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
        capture->bits_per_sample = 32;
        capture->valid_bits = 32;
        capture->is_float = true;
        format_found = true;
      }
    } else if (capture->format == WASAPI_SAMPLE_FORMAT_S24) {
      wfx.Format.wBitsPerSample = 24;
      wfx.Format.nBlockAlign = 3 * capture->channels;
      wfx.Format.nAvgBytesPerSec =
          capture->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 24;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      hr = IAudioClient_IsFormatSupported(capture->client, mode,
                                          (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
        capture->bits_per_sample = 24;
        capture->valid_bits = 24;
        capture->is_float = false;
        format_found = true;
      } else {
        wfx.Format.wBitsPerSample = 32;
        wfx.Format.nBlockAlign = 4 * capture->channels;
        wfx.Format.nAvgBytesPerSec =
            capture->sample_rate * wfx.Format.nBlockAlign;
        wfx.Samples.wValidBitsPerSample = 24;
        wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        hr = IAudioClient_IsFormatSupported(capture->client, mode,
                                            (WAVEFORMATEX*)&wfx, NULL);
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
  if (mode == AUDCLNT_SHAREMODE_SHARED) {
    flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  }

  hr = IAudioClient_Initialize(
      capture->client, mode, flags, duration,
      (mode == AUDCLNT_SHAREMODE_EXCLUSIVE) ? duration : 0, (WAVEFORMATEX*)&wfx,
      NULL);
  if (FAILED(hr)) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "Failed to initialize IAudioClient (Capture): hr=0x%08lX", (unsigned long)hr);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
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

  hr =
      IAudioClient_GetBufferSize(capture->client, &capture->buffer_frame_count);
  hr = IAudioClient_GetService(capture->client, &IID_IAudioCaptureClient,
                               (void**)&capture->capture_client);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to get IAudioCaptureClient");
    goto error_cleanup;
  }

  capture->residual_chunk = audio_chunk_create(capture->channels, capture->chunk_size * 4);
  if (!capture->residual_chunk) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate WASAPI capture residual buffer");
    goto error_cleanup;
  }
  capture->residual_frames = 0;
  capture->residual_offset = 0;

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
  if (capture->residual_chunk) {
    audio_chunk_free(capture->residual_chunk);
    capture->residual_chunk = NULL;
  }
  return false;
}

bool wasapi_capture_read(wasapi_capture_t* capture, size_t frames,
                         audio_chunk_t* chunk, backend_error_t* err) {
  if (audio_chunk_get_channels(chunk) < (size_t)capture->channels) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INVALID_CHANNELS,
                         "Chunk channels count does not match capture channels");
    }
    return false;
  }
  size_t frames_read = 0;
  DWORD start_time = GetTickCount();

  // 1. Consume any remaining samples from the residual buffer
  if (capture->residual_frames > 0) {
    size_t to_copy = frames - frames_read;
    if (to_copy > capture->residual_frames) {
      to_copy = capture->residual_frames;
    }
    for (size_t ch = 0; ch < (size_t)capture->channels; ch++) {
      double* dst = audio_chunk_get_channel(chunk, ch);
      const double* src = audio_chunk_get_channel(capture->residual_chunk, ch);
      if (dst && src) {
        memcpy(dst + frames_read, src + capture->residual_offset, to_copy * sizeof(double));
      }
    }
    frames_read += to_copy;
    capture->residual_frames -= to_copy;
    capture->residual_offset += to_copy;
    if (capture->residual_frames == 0) {
      capture->residual_offset = 0;
    }
  }

  // 2. Loop to read packets from WASAPI device
  while (frames_read < frames) {
    if (GetTickCount() - start_time > 1000) {
      if (err) {
        backend_error_init(err, BACKEND_ERROR_READ_ERROR, "WASAPI capture timeout (device stalled)");
      }
      return false;
    }

    // Check the size of the next available packet in the capture buffer.
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
      // Retrieve the pointer to the data packet in the shared buffer.
      hr = IAudioCaptureClient_GetBuffer(capture->capture_client, &data,
                                         &num_frames, &flags, NULL, NULL);
      if (SUCCEEDED(hr) && data) {
        start_time = GetTickCount(); // progress made, reset timeout
        UINT32 to_copy = frames - frames_read;
        if (to_copy >= num_frames) {
          // Consume the entire packet
          // Decode the raw WASAPI format into the audio chunk.
          decode_samples_from_wasapi(
              chunk, frames_read, data, num_frames, capture->channels, flags,
              capture->bits_per_sample, capture->valid_bits, capture->is_float);
          frames_read += num_frames;
        } else {
          // Consume a portion, buffer the rest
          decode_samples_from_wasapi(
              chunk, frames_read, data, to_copy, capture->channels, flags,
              capture->bits_per_sample, capture->valid_bits, capture->is_float);

          size_t sample_size = (capture->bits_per_sample > 0) ? ((size_t)capture->bits_per_sample / 8) : 4;
          const BYTE* extra_data = data + (size_t)to_copy * capture->channels * sample_size;
          UINT32 extra_frames = num_frames - to_copy;

          if (extra_frames > capture->chunk_size * 4) {
            extra_frames = capture->chunk_size * 4;
          }

          decode_samples_from_wasapi(
              capture->residual_chunk, 0, extra_data, extra_frames, capture->channels, flags,
              capture->bits_per_sample, capture->valid_bits, capture->is_float);

          capture->residual_frames = extra_frames;
          capture->residual_offset = 0;
          frames_read += to_copy;
        }
        // Release the buffer back to WASAPI.
        IAudioCaptureClient_ReleaseBuffer(capture->capture_client, num_frames);
      }
    } else {
      // If no data is available, wait before checking again.
      if (capture->polling) {
        Sleep(1);
      } else {
        // Wait for the event to be signaled by WASAPI indicating data is ready.
        if (WaitForSingleObject(capture->event, 100) != WAIT_OBJECT_0) {
          // Timeout or error wait
        }
      }
    }
  }

  audio_chunk_set_valid_frames(chunk, frames);
  return true;
}

void wasapi_capture_close(wasapi_capture_t* capture) {
  if (!capture) return;
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
  if (capture->residual_chunk) {
    audio_chunk_free(capture->residual_chunk);
    capture->residual_chunk = NULL;
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

void wasapi_capture_destroy(wasapi_capture_t* capture) {
  if (capture) {
    wasapi_capture_close(capture);
    free(capture);
  }
}

// MARK: - Playback Backend implementation

/**
 * @brief Vtable adapter to open the WASAPI playback stream.
 * @param ctx Pointer to the wasapi_playback_t context.
 * @param err Pointer to backend_error_t to receive error details.
 * @return true if successful, false otherwise.
 */
static bool play_vtable_open(void* ctx, backend_error_t* err) {
  return wasapi_playback_open((wasapi_playback_t*)ctx, err);
}

/**
 * @brief Vtable adapter to write a chunk of audio to the WASAPI playback
 * stream.
 * @param ctx Pointer to the wasapi_playback_t context.
 * @param chunk Pointer to audio_chunk_t containing the data to write.
 * @param err Pointer to backend_error_t to receive error details.
 * @return true if successful, false otherwise.
 */
static bool play_vtable_write(void* ctx, const audio_chunk_t* chunk,
                              backend_error_t* err) {
  return wasapi_playback_write((wasapi_playback_t*)ctx, chunk, err);
}

/**
 * @brief Vtable adapter to close the WASAPI playback stream.
 * @param ctx Pointer to the wasapi_playback_t context.
 */
static void play_vtable_close(void* ctx) {
  wasapi_playback_close((wasapi_playback_t*)ctx);
}

/**
 * @brief Vtable adapter to get the current playback buffer level in frames.
 * @param ctx Pointer to the wasapi_playback_t context.
 * @return Buffer level in frames (padding).
 */
static size_t play_vtable_get_buffer_level(void* ctx) {
  return wasapi_playback_get_buffer_level((wasapi_playback_t*)ctx);
}

/**
 * @brief Vtable adapter to check for pending rate changes.
 * @note WASAPI backend does not support dynamic rate changes.
 * @param ctx Pointer to the wasapi_playback_t context.
 * @param out_rate Pointer to store the new rate (unused).
 * @return Always false.
 */
static bool play_vtable_get_pending_rate_change(void* ctx, double* out_rate) {
  return wasapi_playback_get_pending_rate_change((wasapi_playback_t*)ctx,
                                                 out_rate);
}

/**
 * @brief Vtable adapter to prefill the playback buffer with silence.
 * @param ctx Pointer to the wasapi_playback_t context.
 * @param frames Number of silence frames to write.
 * @param err Pointer to backend_error_t to receive error details.
 * @return true if successful, false otherwise.
 */
static bool play_vtable_prefill_silence(void* ctx, size_t frames,
                                        backend_error_t* err) {
  return wasapi_playback_prefill_silence((wasapi_playback_t*)ctx, frames, err);
}

/**
 * @brief Vtable adapter to check if playback is paused.
 * @param ctx Pointer to the wasapi_playback_t context.
 * @return true if paused, false otherwise.
 */
static bool play_vtable_get_is_paused(void* ctx) {
  return wasapi_playback_get_is_paused((wasapi_playback_t*)ctx);
}

/**
 * @brief Vtable adapter to set the paused state of playback.
 * @param ctx Pointer to the wasapi_playback_t context.
 * @param paused Desired paused state.
 */
static void play_vtable_set_is_paused(void* ctx, bool paused) {
  wasapi_playback_set_is_paused((wasapi_playback_t*)ctx, paused);
}

/**
 * @brief Vtable adapter to destroy the WASAPI playback context.
 * @param ctx Pointer to the wasapi_playback_t context.
 */
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

  if (config->cfg.wasapi.has_device &&
      strcmp(config->cfg.wasapi.device, "default") != 0) {
    snprintf(playback->device, sizeof(playback->device), "%s",
             config->cfg.wasapi.device);
  } else {
    playback->device[0] = '\0';
  }

  playback->sample_rate = sample_rate;
  playback->channels = config->cfg.wasapi.channels;
  playback->chunk_size = chunk_size;
  playback->format = config->cfg.wasapi.format;
  playback->exclusive = config->cfg.wasapi.exclusive;
  playback->polling =
      config->cfg.wasapi.has_polling ? config->cfg.wasapi.polling : false;

  atomic_init(&playback->paused, false);
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
  // Initialize COM library for multithreaded operations.
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

  // Retrieve IMMDevice. If no device name is specified, get default.
  // Otherwise, enumerate endpoints and match friendly name or ID.
  playback->mm_device =
      wasapi_find_device_by_name(playback->enumerator, playback->device, false);

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

  // Set up the wave format structure.
  // We use WAVEFORMATEXTENSIBLE which is required for formats with >2 channels
  // or high precision sample types (>16-bit).
  WAVEFORMATEXTENSIBLE wfx;
  memset(&wfx, 0, sizeof(wfx));
  wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wfx.Format.nChannels = playback->channels;
  wfx.Format.nSamplesPerSec = playback->sample_rate;
  wfx.Format.cbSize = 22;
  wfx.dwChannelMask = (playback->channels == 2)
                          ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                          : 0;

  // WASAPI Shared mode usually only supports IEEE Float formats.
  // Exclusive mode can support raw PCM.
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
      wfx.Format.nAvgBytesPerSec =
          playback->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 16;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      hr = IAudioClient_IsFormatSupported(playback->client, mode,
                                          (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
        playback->bits_per_sample = 16;
        playback->valid_bits = 16;
        playback->is_float = false;
        format_found = true;
      }
    } else if (playback->format == WASAPI_SAMPLE_FORMAT_S32) {
      wfx.Format.wBitsPerSample = 32;
      wfx.Format.nBlockAlign = 4 * playback->channels;
      wfx.Format.nAvgBytesPerSec =
          playback->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 32;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      hr = IAudioClient_IsFormatSupported(playback->client, mode,
                                          (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
        playback->bits_per_sample = 32;
        playback->valid_bits = 32;
        playback->is_float = false;
        format_found = true;
      }
    } else if (playback->format == WASAPI_SAMPLE_FORMAT_F32) {
      wfx.Format.wBitsPerSample = 32;
      wfx.Format.nBlockAlign = 4 * playback->channels;
      wfx.Format.nAvgBytesPerSec =
          playback->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 32;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
      hr = IAudioClient_IsFormatSupported(playback->client, mode,
                                          (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
        playback->bits_per_sample = 32;
        playback->valid_bits = 32;
        playback->is_float = true;
        format_found = true;
      }
    } else if (playback->format == WASAPI_SAMPLE_FORMAT_S24) {
      wfx.Format.wBitsPerSample = 24;
      wfx.Format.nBlockAlign = 3 * playback->channels;
      wfx.Format.nAvgBytesPerSec =
          playback->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 24;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      hr = IAudioClient_IsFormatSupported(playback->client, mode,
                                          (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
        playback->bits_per_sample = 24;
        playback->valid_bits = 24;
        playback->is_float = false;
        format_found = true;
      } else {
        wfx.Format.wBitsPerSample = 32;
        wfx.Format.nBlockAlign = 4 * playback->channels;
        wfx.Format.nAvgBytesPerSec =
            playback->sample_rate * wfx.Format.nBlockAlign;
        wfx.Samples.wValidBitsPerSample = 24;
        wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        hr = IAudioClient_IsFormatSupported(playback->client, mode,
                                            (WAVEFORMATEX*)&wfx, NULL);
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
  if (mode == AUDCLNT_SHAREMODE_SHARED) {
    flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  }

  hr = IAudioClient_Initialize(playback->client, mode, flags, duration,
                               playback->exclusive ? duration : 0,
                               (WAVEFORMATEX*)&wfx, NULL);
  if (FAILED(hr)) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "Failed to initialize IAudioClient (Playback): hr=0x%08lX", (unsigned long)hr);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
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
  if (atomic_load_explicit(&playback->paused, memory_order_acquire))
    return true;

  if (audio_chunk_get_channels(chunk) < (size_t)playback->channels) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INVALID_CHANNELS,
                         "Chunk channels count does not match playback channels");
    }
    return false;
  }

  size_t frames_written = 0;
  size_t total_frames = audio_chunk_get_valid_frames(chunk);
  DWORD start_time = GetTickCount();

  // Keep writing audio packets until all frames from the input chunk are
  // written.
  while (frames_written < total_frames) {
    if (GetTickCount() - start_time > 1000) {
      if (err) {
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, "WASAPI playback timeout (device stalled)");
      }
      return false;
    }

    UINT32 padding = 0;
    // Get the amount of data already buffered in the endpoint.
    HRESULT hr = IAudioClient_GetCurrentPadding(playback->client, &padding);
    if (FAILED(hr)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "Failed to get padding");
      return false;
    }

    // Calculate how much space is left in the endpoint buffer.
    UINT32 available_frames = playback->buffer_frame_count - padding;
    UINT32 to_write = total_frames - frames_written;
    if (to_write > available_frames) {
      to_write = available_frames;
    }

    if (to_write > 0) {
      BYTE* data = NULL;
      // Get a pointer to the buffer space where we can write the audio data.
      hr = IAudioRenderClient_GetBuffer(playback->render_client, to_write,
                                        &data);
      if (SUCCEEDED(hr) && data) {
        start_time = GetTickCount(); // progress made, reset timeout
        // Encode and copy the samples from the audio chunk to the WASAPI
        // buffer.
        encode_samples_to_wasapi(data, chunk, frames_written, to_write,
                                 playback->channels, playback->bits_per_sample,
                                 playback->valid_bits, playback->is_float);
        // Release the buffer back to WASAPI to be scheduled for playback.
        IAudioRenderClient_ReleaseBuffer(playback->render_client, to_write, 0);
        frames_written += to_write;
      }
    } else {
      // If buffer is full, wait before trying to write again.
      if (playback->polling) {
        Sleep(1);
      } else {
        // Wait for the event signaled by WASAPI when it has processed some
        // data.
        if (WaitForSingleObject(playback->event, 100) != WAIT_OBJECT_0) {
          // Timeout or error wait
        }
      }
    }
  }
  return true;
}

void wasapi_playback_close(wasapi_playback_t* playback) {
  if (!playback) return;
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
  if (!playback) return false;
  return atomic_load_explicit(&playback->paused, memory_order_acquire);
}

void wasapi_playback_set_is_paused(wasapi_playback_t* playback, bool paused) {
  if (!playback) return;
  atomic_store_explicit(&playback->paused, paused, memory_order_release);
}

void wasapi_playback_destroy(wasapi_playback_t* playback) {
  if (playback) {
    wasapi_playback_close(playback);
    free(playback);
  }
}

#endif  // ENABLE_WASAPI
