// Audio backend protocols.
//
// `ProcessingState` and `ProcessingStopReason` — used by both the
// engine internals and the public actor — live in `Engine/DSPEngine.swift`.

#include "audio_backend.h"
#if defined(ENABLE_COREAUDIO)
#include "core_audio_capture.h"
#include "core_audio_playback.h"
#endif
#if defined(ENABLE_ALSA)
#include "alsa_capture.h"
#include "alsa_playback.h"
#endif
#if defined(ENABLE_PIPEWIRE)
#include "pipewire_backend.h"
#endif
#if defined(ENABLE_PULSE)
#include "pulse_backend.h"
#endif
#if defined(ENABLE_ASIO)
#include "asio_backend.h"
#endif
#if defined(ENABLE_WASAPI)
#include "wasapi_backend.h"
#endif
#if defined(ENABLE_JACK)
#include "jack_backend.h"
#endif
#if defined(ENABLE_BLUEZ)
#include "bluez_backend.h"
#endif
#include <stdlib.h>

#include "file_backend.h"
#include "generator_capture.h"

capture_backend_t* create_capture_backend(const capture_device_config_t* config,
                                          int sample_rate, int chunk_size,
                                          bool full_duplex,
                                          processing_parameters_t* params,
                                          backend_error_t* err) {
#if !defined(ENABLE_ASIO)
  (void)full_duplex;
#endif
  if (!config) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Config is NULL");
    return NULL;
  }
  switch (config->type) {
#if defined(ENABLE_COREAUDIO)
    case AUDIO_BACKEND_TYPE_CORE_AUDIO:
      return core_audio_capture_create(config, sample_rate, chunk_size, err);
#endif
#if defined(ENABLE_ALSA)
    case AUDIO_BACKEND_TYPE_ALSA:
      return alsa_capture_create(config, sample_rate, chunk_size, params, err);
#endif
#if defined(ENABLE_PULSE)
    case AUDIO_BACKEND_TYPE_PULSE_AUDIO:
      return pulse_capture_create(config, sample_rate, chunk_size, params, err);
#endif
#if defined(ENABLE_PIPEWIRE)
    case AUDIO_BACKEND_TYPE_PIPEWIRE:
      return pipewire_capture_create(config, sample_rate, chunk_size, params,
                                     err);
#endif
#if defined(ENABLE_WASAPI)
    case AUDIO_BACKEND_TYPE_WASAPI:
      return wasapi_capture_create(config, sample_rate, chunk_size, params,
                                   err);
#endif
#if defined(ENABLE_ASIO)
    case AUDIO_BACKEND_TYPE_ASIO:
      return asio_capture_new(config, sample_rate, chunk_size, full_duplex,
                              err);
#endif
#if defined(ENABLE_JACK)
    case AUDIO_BACKEND_TYPE_JACK:
      return jack_capture_create(config, sample_rate, chunk_size, params, err);
#endif
#if defined(ENABLE_BLUEZ)
    case AUDIO_BACKEND_TYPE_BLUEZ:
      return bluez_capture_create(config, sample_rate, chunk_size, params, err);
#endif
    case AUDIO_BACKEND_TYPE_GENERATOR:
      return generator_capture_create(config, sample_rate, chunk_size, params,
                                      err);
    case AUDIO_BACKEND_TYPE_FILE:
    case AUDIO_BACKEND_TYPE_STDIN_OUT:
      return file_capture_create(config, sample_rate, chunk_size, params, err);
    default:
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Unsupported capture backend type");
      return NULL;
  }
}

playback_backend_t* create_playback_backend(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params, backend_error_t* err) {
#if !defined(ENABLE_ASIO)
  (void)full_duplex;
#endif
  if (!config) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Config is NULL");
    return NULL;
  }
  switch (config->type) {
#if defined(ENABLE_COREAUDIO)
    case AUDIO_BACKEND_TYPE_CORE_AUDIO:
      return core_audio_playback_create(config, sample_rate, chunk_size, err);
#endif
#if defined(ENABLE_ALSA)
    case AUDIO_BACKEND_TYPE_ALSA:
      return alsa_playback_create(config, sample_rate, chunk_size, params, err);
#endif
#if defined(ENABLE_PULSE)
    case AUDIO_BACKEND_TYPE_PULSE_AUDIO:
      return pulse_playback_create(config, sample_rate, chunk_size, params,
                                   err);
#endif
#if defined(ENABLE_PIPEWIRE)
    case AUDIO_BACKEND_TYPE_PIPEWIRE:
      return pipewire_playback_create(config, sample_rate, chunk_size, params,
                                      err);
#endif
#if defined(ENABLE_WASAPI)
    case AUDIO_BACKEND_TYPE_WASAPI:
      return wasapi_playback_create(config, sample_rate, chunk_size, params,
                                    err);
#endif
#if defined(ENABLE_ASIO)
    case AUDIO_BACKEND_TYPE_ASIO:
      return asio_playback_new(config, sample_rate, chunk_size, full_duplex,
                               err);
#endif
#if defined(ENABLE_JACK)
    case AUDIO_BACKEND_TYPE_JACK:
      return jack_playback_create(config, sample_rate, chunk_size, params, err);
#endif
    case AUDIO_BACKEND_TYPE_FILE:
    case AUDIO_BACKEND_TYPE_STDIN_OUT:
      return file_playback_create(config, sample_rate, chunk_size, params, err);
    default:
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Unsupported playback backend type");
      return NULL;
  }
}

/// Open the capture device
bool capture_backend_open(capture_backend_t* backend, backend_error_t* err) {
  if (!backend || !backend->vtable || !backend->vtable->open) return false;
  return backend->vtable->open(backend->ctx, err);
}

/// Read a chunk of audio into the provided buffer. Returns false on
/// end-of-stream or no data.
bool capture_backend_read(capture_backend_t* backend, size_t frames,
                          audio_chunk_t* chunk, backend_error_t* err) {
  if (!backend || !backend->vtable || !backend->vtable->read) return false;
  return backend->vtable->read(backend->ctx, frames, chunk, err);
}

/// Close the capture device
void capture_backend_close(capture_backend_t* backend) {
  if (!backend || !backend->vtable || !backend->vtable->close) return;
  backend->vtable->close(backend->ctx);
}

/// Get pending sample rate change detected on the capture device.
bool capture_backend_get_pending_rate_change(capture_backend_t* backend,
                                             double* out_rate) {
  if (!backend || !backend->vtable || !backend->vtable->get_pending_rate_change)
    return false;
  return backend->vtable->get_pending_rate_change(backend->ctx, out_rate);
}

/// Check if the capture device supports clock-pitch correction.
bool capture_backend_pitch_control_supported(capture_backend_t* backend) {
  if (!backend || !backend->vtable ||
      !backend->vtable->is_pitch_control_supported)
    return false;
  return backend->vtable->is_pitch_control_supported(backend->ctx);
}

/// Apply a clock-pitch correction to the capture device.
void capture_backend_set_pitch(capture_backend_t* backend, double multiplier) {
  if (!backend || !backend->vtable || !backend->vtable->set_pitch) return;
  backend->vtable->set_pitch(backend->ctx, multiplier);
}

/// Wait for new samples to become available, up to the given timeout.
bool capture_backend_wait(capture_backend_t* backend, uint32_t timeout_ms) {
  if (!backend || !backend->vtable || !backend->vtable->wait_for_data)
    return false;
  return backend->vtable->wait_for_data(backend->ctx, timeout_ms);
}

/// Notify the capture backend of the paused state.
void capture_backend_set_is_paused(capture_backend_t* backend, bool paused) {
  if (!backend || !backend->vtable || !backend->vtable->set_is_paused) return;
  backend->vtable->set_is_paused(backend->ctx, paused);
}

/// Destroy and free the capture backend.
void capture_backend_free(capture_backend_t* backend) {
  if (!backend) return;
  if (backend->vtable && backend->vtable->destroy) {
    backend->vtable->destroy(backend->ctx);
  }
  free(backend);
}

/// Open the playback device
bool playback_backend_open(playback_backend_t* backend, backend_error_t* err) {
  if (!backend || !backend->vtable || !backend->vtable->open) return false;
  return backend->vtable->open(backend->ctx, err);
}

/// Write a chunk of audio
bool playback_backend_write(playback_backend_t* backend,
                            const audio_chunk_t* chunk, backend_error_t* err) {
  if (!backend || !backend->vtable || !backend->vtable->write) return false;
  return backend->vtable->write(backend->ctx, chunk, err);
}

/// Close the playback device
void playback_backend_close(playback_backend_t* backend) {
  if (!backend || !backend->vtable || !backend->vtable->close) return;
  backend->vtable->close(backend->ctx);
}

/// Get the current playback buffer level in samples
size_t playback_backend_get_buffer_level(playback_backend_t* backend) {
  if (!backend || !backend->vtable || !backend->vtable->get_buffer_level)
    return 0;
  return backend->vtable->get_buffer_level(backend->ctx);
}

/// Get pending sample rate change detected on the playback device.
bool playback_backend_get_pending_rate_change(playback_backend_t* backend,
                                              double* out_rate) {
  if (!backend || !backend->vtable || !backend->vtable->get_pending_rate_change)
    return false;
  return backend->vtable->get_pending_rate_change(backend->ctx, out_rate);
}

/// Push zero samples per channel into the output ring before first real chunk
/// arrives.
bool playback_backend_prefill_silence(playback_backend_t* backend,
                                      size_t frames, backend_error_t* err) {
  if (!backend || !backend->vtable || !backend->vtable->prefill_silence)
    return false;
  return backend->vtable->prefill_silence(backend->ctx, frames, err);
}

/// Get paused flag status.
bool playback_backend_get_is_paused(playback_backend_t* backend) {
  if (!backend || !backend->vtable || !backend->vtable->get_is_paused)
    return false;
  return backend->vtable->get_is_paused(backend->ctx);
}

/// Set paused flag status.
void playback_backend_set_is_paused(playback_backend_t* backend, bool paused) {
  if (!backend || !backend->vtable || !backend->vtable->set_is_paused) return;
  backend->vtable->set_is_paused(backend->ctx, paused);
}

bool playback_backend_pitch_control_supported(playback_backend_t* backend) {
  if (!backend || !backend->vtable || !backend->vtable->pitch_control_supported)
    return false;
  return backend->vtable->pitch_control_supported(backend->ctx);
}

void playback_backend_set_pitch(playback_backend_t* backend,
                                double multiplier) {
  if (!backend || !backend->vtable || !backend->vtable->set_pitch) return;
  backend->vtable->set_pitch(backend->ctx, multiplier);
}

/// Destroy and free the playback backend.
void playback_backend_free(playback_backend_t* backend) {
  if (!backend) return;
  if (backend->vtable && backend->vtable->destroy) {
    backend->vtable->destroy(backend->ctx);
  }
  free(backend);
}
