#if defined(ENABLE_PULSE)

#include "pulse_backend.h"

#include <pulse/error.h>
#include <pulse/simple.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "Logging/app_logger.h"

struct pulse_capture {
  char device[256];
  int sample_rate;
  int channels;
  int chunk_size;
  pa_simple* s;
  uint8_t* raw_buf;
  size_t raw_buf_size;
};

struct pulse_playback {
  char device[256];
  int sample_rate;
  int channels;
  int chunk_size;
  pa_simple* s;
  uint8_t* raw_buf;
  size_t raw_buf_size;
  size_t total_bytes_written;
  _Atomic bool paused;
};

// MARK: - Pulse Capture Backend implementation

/**
 * @brief Vtable adapter to open the PulseAudio capture stream.
 * @param ctx Pointer to the pulse_capture_t context.
 * @param err Pointer to backend_error_t to receive error details.
 * @return true if successful, false otherwise.
 */
static bool cap_vtable_open(void* ctx, backend_error_t* err) {
  return pulse_capture_open((pulse_capture_t*)ctx, err);
}

/**
 * @brief Vtable adapter to read frames from the PulseAudio capture stream.
 * @param ctx Pointer to the pulse_capture_t context.
 * @param frames Number of frames to read.
 * @param chunk Pointer to audio_chunk_t to store the read audio data.
 * @param err Pointer to backend_error_t to receive error details.
 * @return true if successful, false otherwise.
 */
static bool cap_vtable_read(void* ctx, size_t frames, audio_chunk_t* chunk,
                            backend_error_t* err) {
  return pulse_capture_read((pulse_capture_t*)ctx, frames, chunk, err);
}

/**
 * @brief Vtable adapter to close the PulseAudio capture stream.
 * @param ctx Pointer to the pulse_capture_t context.
 */
static void cap_vtable_close(void* ctx) {
  pulse_capture_close((pulse_capture_t*)ctx);
}

/**
 * @brief Vtable adapter to check for pending rate changes.
 * @note PulseAudio backend does not support dynamic rate changes.
 * @param ctx Pointer to the pulse_capture_t context.
 * @param out_rate Pointer to store the new rate (unused).
 * @return Always false.
 */
static bool cap_vtable_get_pending_rate_change(void* ctx, double* out_rate) {
  return pulse_capture_get_pending_rate_change((pulse_capture_t*)ctx, out_rate);
}

/**
 * @brief Vtable adapter to check if pitch control is supported.
 * @note PulseAudio backend does not support pitch control.
 * @param ctx Pointer to the pulse_capture_t context.
 * @return Always false.
 */
static bool cap_vtable_is_pitch_control_supported(void* ctx) {
  return pulse_capture_pitch_control_supported((pulse_capture_t*)ctx);
}

/**
 * @brief Vtable adapter to set pitch multiplier.
 * @note PulseAudio backend does not support pitch control (no-op).
 * @param ctx Pointer to the pulse_capture_t context.
 * @param multiplier The pitch multiplier.
 */
static void cap_vtable_set_pitch(void* ctx, double multiplier) {
  pulse_capture_set_pitch((pulse_capture_t*)ctx, multiplier);
}

/**
 * @brief Vtable adapter to wait for data to become available.
 * @param ctx Pointer to the pulse_capture_t context.
 * @param timeout_ms Timeout in milliseconds.
 * @return true after waiting.
 */
static bool cap_vtable_wait_for_data(void* ctx, uint32_t timeout_ms) {
  return pulse_capture_wait((pulse_capture_t*)ctx, timeout_ms);
}

/**
 * @brief Vtable adapter to destroy the PulseAudio capture context.
 * @param ctx Pointer to the pulse_capture_t context.
 */
static void cap_vtable_destroy(void* ctx) {
  pulse_capture_destroy((pulse_capture_t*)ctx);
}

static const capture_backend_vtable_t pulse_capture_vtable = {
    .open = cap_vtable_open,
    .read = cap_vtable_read,
    .close = cap_vtable_close,
    .get_pending_rate_change = cap_vtable_get_pending_rate_change,
    .is_pitch_control_supported = cap_vtable_is_pitch_control_supported,
    .set_pitch = cap_vtable_set_pitch,
    .wait_for_data = cap_vtable_wait_for_data,
    .destroy = cap_vtable_destroy};

capture_backend_t* pulse_capture_create(const capture_device_config_t* config,
                                        int sample_rate, int chunk_size,
                                        processing_parameters_t* params,
                                        backend_error_t* err) {
  (void)params;
  (void)err;
  pulse_capture_t* capture =
      (pulse_capture_t*)calloc(1, sizeof(pulse_capture_t));
  if (!capture) return NULL;

  if (strlen(config->cfg.pulse.device) > 0 &&
      strcmp(config->cfg.pulse.device, "default") != 0) {
    snprintf(capture->device, sizeof(capture->device), "%s",
             config->cfg.pulse.device);
  } else {
    capture->device[0] = '\0';  // default device
  }

  capture->sample_rate = sample_rate;
  capture->channels = config->cfg.pulse.channels;
  capture->chunk_size = chunk_size;

  capture_backend_t* backend =
      (capture_backend_t*)calloc(1, sizeof(capture_backend_t));
  if (!backend) {
    free(capture);
    return NULL;
  }
  backend->ctx = capture;
  backend->vtable = &pulse_capture_vtable;
  return backend;
}

bool pulse_capture_open(pulse_capture_t* capture, backend_error_t* err) {
  pa_sample_spec ss = {.format = PA_SAMPLE_FLOAT32LE,
                       .rate = (uint32_t)capture->sample_rate,
                       .channels = (uint8_t)capture->channels};

  // Configure PulseAudio buffer attributes.
  // We request default values for maxlength, tlength, prebuf, and minreq by
  // setting them to -1. fragsize is set to the size of a single frame (channels
  // * sizeof(float)) to minimize latency by requesting small fragments from the
  // server.
  pa_buffer_attr attr = {
      .maxlength = (uint32_t)-1,
      .tlength = (uint32_t)-1,
      .prebuf = (uint32_t)-1,
      .minreq = (uint32_t)-1,
      .fragsize = (uint32_t)(capture->channels * sizeof(float))};

  int error;
  capture->s =
      pa_simple_new(NULL, "CDSP-Monitor", PA_STREAM_RECORD,
                    capture->device[0] != '\0' ? capture->device : NULL,
                    "Capture", &ss, NULL, &attr, &error);

  if (!capture->s) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         pa_strerror(error));
    return false;
  }

  // Allocate raw buffer for holding interleaved float data read from
  // PulseAudio.
  capture->raw_buf_size =
      capture->chunk_size * capture->channels * sizeof(float);
  capture->raw_buf = (uint8_t*)malloc(capture->raw_buf_size);
  if (!capture->raw_buf) {
    pa_simple_free(capture->s);
    capture->s = NULL;
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Memory allocation failure");
    return false;
  }

  logger_t logger = logger_create("dsp.backend.pulse");
  logger_info(
      &logger, "Opened PulseAudio capture: device=%s, rate=%d, channels=%d",
      log_arg_string(capture->device[0] != '\0' ? capture->device : "default"),
      log_arg_int((int64_t)capture->sample_rate),
      log_arg_int((int64_t)capture->channels), log_arg_none());

  return true;
}

bool pulse_capture_read(pulse_capture_t* capture, size_t frames,
                        audio_chunk_t* chunk, backend_error_t* err) {
  if (audio_chunk_get_channels(chunk) < (size_t)capture->channels) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INVALID_CHANNELS,
                         "Chunk channels count does not match capture channels");
    }
    return false;
  }
  size_t bytes_to_read = frames * capture->channels * sizeof(float);
  // Dynamically resize internal buffer if requested frame count exceeds current
  // size.
  if (bytes_to_read > capture->raw_buf_size) {
    uint8_t* new_buf = (uint8_t*)realloc(capture->raw_buf, bytes_to_read);
    if (!new_buf) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                           "Failed to reallocate PulseAudio capture buffer");
      return false;
    }
    capture->raw_buf = new_buf;
    capture->raw_buf_size = bytes_to_read;
  }

  int error;
  if (pa_simple_read(capture->s, capture->raw_buf, bytes_to_read, &error) < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR, pa_strerror(error));
    return false;
  }

  // Convert interleaved 32-bit float samples from PulseAudio
  // to deinterleaved double samples in the output audio chunk.
  float* src = (float*)capture->raw_buf;
  for (size_t f = 0; f < frames; f++) {
    for (int c = 0; c < capture->channels; c++) {
      audio_chunk_get_channel(chunk, c)[f] =
          (double)src[f * capture->channels + c];
    }
  }

  audio_chunk_set_valid_frames(chunk, frames);
  return true;
}

void pulse_capture_close(pulse_capture_t* capture) {
  if (!capture) return;
  if (capture->s) {
    pa_simple_free(capture->s);
    capture->s = NULL;
  }
  if (capture->raw_buf) {
    free(capture->raw_buf);
    capture->raw_buf = NULL;
  }
}

bool pulse_capture_get_pending_rate_change(pulse_capture_t* capture,
                                           double* out_rate) {
  (void)capture;
  (void)out_rate;
  return false;
}

bool pulse_capture_pitch_control_supported(pulse_capture_t* capture) {
  (void)capture;
  return false;
}

void pulse_capture_set_pitch(pulse_capture_t* capture, double multiplier) {
  (void)capture;
  (void)multiplier;
}

bool pulse_capture_wait(pulse_capture_t* capture, uint32_t timeout_ms) {
  (void)capture;
  // Pulse Simple API blocks natively during read/write.
  struct timespec req = {.tv_sec = (time_t)(timeout_ms / 1000),
                         .tv_nsec = (long)((timeout_ms % 1000) * 1000000L)};
  nanosleep(&req, NULL);
  return true;
}

void pulse_capture_destroy(pulse_capture_t* capture) {
  if (capture) {
    pulse_capture_close(capture);
    free(capture);
  }
}

// MARK: - Pulse Playback Backend implementation

/**
 * @brief Vtable adapter to open the PulseAudio playback stream.
 * @param ctx Pointer to the pulse_playback_t context.
 * @param err Pointer to backend_error_t to receive error details.
 * @return true if successful, false otherwise.
 */
static bool play_vtable_open(void* ctx, backend_error_t* err) {
  return pulse_playback_open((pulse_playback_t*)ctx, err);
}

/**
 * @brief Vtable adapter to write a chunk of audio to the PulseAudio playback
 * stream.
 * @param ctx Pointer to the pulse_playback_t context.
 * @param chunk Pointer to audio_chunk_t containing the data to write.
 * @param err Pointer to backend_error_t to receive error details.
 * @return true if successful, false otherwise.
 */
static bool play_vtable_write(void* ctx, const audio_chunk_t* chunk,
                              backend_error_t* err) {
  return pulse_playback_write((pulse_playback_t*)ctx, chunk, err);
}

/**
 * @brief Vtable adapter to close the PulseAudio playback stream.
 * @param ctx Pointer to the pulse_playback_t context.
 */
static void play_vtable_close(void* ctx) {
  pulse_playback_close((pulse_playback_t*)ctx);
}

/**
 * @brief Vtable adapter to get the current playback buffer level in frames.
 * @param ctx Pointer to the pulse_playback_t context.
 * @return Buffer level in frames.
 */
static size_t play_vtable_get_buffer_level(void* ctx) {
  return pulse_playback_get_buffer_level((pulse_playback_t*)ctx);
}

/**
 * @brief Vtable adapter to check for pending rate changes.
 * @note PulseAudio backend does not support dynamic rate changes.
 * @param ctx Pointer to the pulse_playback_t context.
 * @param out_rate Pointer to store the new rate (unused).
 * @return Always false.
 */
static bool play_vtable_get_pending_rate_change(void* ctx, double* out_rate) {
  return pulse_playback_get_pending_rate_change((pulse_playback_t*)ctx,
                                                out_rate);
}

/**
 * @brief Vtable adapter to prefill the playback buffer with silence.
 * @param ctx Pointer to the pulse_playback_t context.
 * @param frames Number of silence frames to write.
 * @param err Pointer to backend_error_t to receive error details.
 * @return true if successful, false otherwise.
 */
static bool play_vtable_prefill_silence(void* ctx, size_t frames,
                                        backend_error_t* err) {
  return pulse_playback_prefill_silence((pulse_playback_t*)ctx, frames, err);
}

/**
 * @brief Vtable adapter to check if playback is paused.
 * @param ctx Pointer to the pulse_playback_t context.
 * @return true if paused, false otherwise.
 */
static bool play_vtable_get_is_paused(void* ctx) {
  return pulse_playback_get_is_paused((pulse_playback_t*)ctx);
}

/**
 * @brief Vtable adapter to set the paused state of playback.
 * @param ctx Pointer to the pulse_playback_t context.
 * @param paused Desired paused state.
 */
static void play_vtable_set_is_paused(void* ctx, bool paused) {
  pulse_playback_set_is_paused((pulse_playback_t*)ctx, paused);
}

/**
 * @brief Vtable adapter to destroy the PulseAudio playback context.
 * @param ctx Pointer to the pulse_playback_t context.
 */
static void play_vtable_destroy(void* ctx) {
  pulse_playback_destroy((pulse_playback_t*)ctx);
}

static const playback_backend_vtable_t pulse_playback_vtable = {
    .open = play_vtable_open,
    .write = play_vtable_write,
    .close = play_vtable_close,
    .get_buffer_level = play_vtable_get_buffer_level,
    .get_pending_rate_change = play_vtable_get_pending_rate_change,
    .prefill_silence = play_vtable_prefill_silence,
    .get_is_paused = play_vtable_get_is_paused,
    .set_is_paused = play_vtable_set_is_paused,
    .destroy = play_vtable_destroy};

playback_backend_t* pulse_playback_create(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    processing_parameters_t* params, backend_error_t* err) {
  (void)params;
  (void)err;
  pulse_playback_t* playback =
      (pulse_playback_t*)calloc(1, sizeof(pulse_playback_t));
  if (!playback) return NULL;

  if (strlen(config->cfg.pulse.device) > 0 &&
      strcmp(config->cfg.pulse.device, "default") != 0) {
    snprintf(playback->device, sizeof(playback->device), "%s",
             config->cfg.pulse.device);
  } else {
    playback->device[0] = '\0';
  }

  playback->sample_rate = sample_rate;
  playback->channels = config->cfg.pulse.channels;
  playback->chunk_size = chunk_size;
  atomic_init(&playback->paused, false);

  playback_backend_t* backend =
      (playback_backend_t*)calloc(1, sizeof(playback_backend_t));
  if (!backend) {
    free(playback);
    return NULL;
  }
  backend->ctx = playback;
  backend->vtable = &pulse_playback_vtable;
  return backend;
}

bool pulse_playback_open(pulse_playback_t* playback, backend_error_t* err) {
  pa_sample_spec ss = {.format = PA_SAMPLE_FLOAT32LE,
                       .rate = (uint32_t)playback->sample_rate,
                       .channels = (uint8_t)playback->channels};

  // Configure PulseAudio buffer attributes.
  // We request default values for maxlength, tlength, and minreq by setting
  // them to -1. prebuf specifies how much data must be in the buffer before
  // starting playback. We set it to one frame size (channels * sizeof(float))
  // to minimize startup latency.
  pa_buffer_attr attr = {
      .maxlength = (uint32_t)-1,
      .tlength = (uint32_t)-1,
      .prebuf = (uint32_t)(playback->channels * sizeof(float)),
      .minreq = (uint32_t)-1,
      .fragsize = (uint32_t)-1};

  int error;
  playback->s =
      pa_simple_new(NULL, "CDSP-Monitor", PA_STREAM_PLAYBACK,
                    playback->device[0] != '\0' ? playback->device : NULL,
                    "Playback", &ss, NULL, &attr, &error);

  if (!playback->s) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         pa_strerror(error));
    return false;
  }

  // Allocate raw buffer for holding interleaved float data to be written to
  // PulseAudio.
  playback->raw_buf_size =
      playback->chunk_size * playback->channels * sizeof(float);
  playback->raw_buf = (uint8_t*)malloc(playback->raw_buf_size);
  if (!playback->raw_buf) {
    pa_simple_free(playback->s);
    playback->s = NULL;
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Memory allocation failure");
    return false;
  }

  playback->paused = false;

  logger_t logger = logger_create("dsp.backend.pulse");
  logger_info(&logger,
              "Opened PulseAudio playback: device=%s, rate=%d, channels=%d",
              log_arg_string(playback->device[0] != '\0' ? playback->device
                                                         : "default"),
              log_arg_int((int64_t)playback->sample_rate),
              log_arg_int((int64_t)playback->channels), log_arg_none());

  return true;
}

bool pulse_playback_write(pulse_playback_t* playback,
                          const audio_chunk_t* chunk, backend_error_t* err) {
  if (audio_chunk_get_channels(chunk) < (size_t)playback->channels) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INVALID_CHANNELS,
                         "Chunk channels count does not match playback channels");
    }
    return false;
  }
  if (atomic_load_explicit(&playback->paused, memory_order_acquire)) {
    return true;
  }

  size_t frames = audio_chunk_get_valid_frames(chunk);
  size_t required_bytes = frames * playback->channels * sizeof(float);
  // Dynamically resize internal buffer if write size exceeds current size.
  if (required_bytes > playback->raw_buf_size) {
    uint8_t* new_buf = (uint8_t*)realloc(playback->raw_buf, required_bytes);
    if (!new_buf) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "Failed to reallocate PulseAudio playback buffer");
      return false;
    }
    playback->raw_buf = new_buf;
    playback->raw_buf_size = required_bytes;
  }

  // Convert deinterleaved double samples from audio chunk
  // to interleaved 32-bit float samples for PulseAudio.
  float* dst = (float*)playback->raw_buf;
  for (size_t f = 0; f < frames; f++) {
    for (int c = 0; c < playback->channels; c++) {
      dst[f * playback->channels + c] =
          (float)audio_chunk_get_channel(chunk, c)[f];
    }
  }

  int error;
  if (pa_simple_write(playback->s, playback->raw_buf, required_bytes, &error) <
      0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, pa_strerror(error));
    return false;
  }

  playback->total_bytes_written += required_bytes;
  return true;
}

void pulse_playback_close(pulse_playback_t* playback) {
  if (!playback) return;
  if (playback->s) {
    int error;
    pa_simple_drain(playback->s, &error);
    pa_simple_free(playback->s);
    playback->s = NULL;
  }
  if (playback->raw_buf) {
    free(playback->raw_buf);
    playback->raw_buf = NULL;
  }
}

size_t pulse_playback_get_buffer_level(pulse_playback_t* playback) {
  if (!playback->s) return 0;
  int error;
  // Get playback latency from PulseAudio in microseconds.
  pa_usec_t latency = pa_simple_get_latency(playback->s, &error);
  if (latency == (pa_usec_t)-1) return 0;
  // Convert microseconds to number of frames based on sample rate.
  return (size_t)((double)latency * (double)playback->sample_rate / 1000000.0);
}

bool pulse_playback_get_pending_rate_change(pulse_playback_t* playback,
                                            double* out_rate) {
  (void)playback;
  (void)out_rate;
  return false;
}

bool pulse_playback_prefill_silence(pulse_playback_t* playback, size_t frames,
                                    backend_error_t* err) {
  if (!playback->s) return false;

  size_t bytes = frames * playback->channels * sizeof(float);
  float* silence = (float*)calloc(frames * playback->channels, sizeof(float));
  if (!silence) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Memory allocation failure");
    return false;
  }

  int error;
  int rc = pa_simple_write(playback->s, silence, bytes, &error);
  free(silence);

  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, pa_strerror(error));
    return false;
  }
  return true;
}

bool pulse_playback_get_is_paused(pulse_playback_t* playback) {
  if (!playback) return false;
  return atomic_load_explicit(&playback->paused, memory_order_acquire);
}

void pulse_playback_set_is_paused(pulse_playback_t* playback, bool paused) {
  if (!playback) return;
  atomic_store_explicit(&playback->paused, paused, memory_order_release);
}

void pulse_playback_destroy(pulse_playback_t* playback) {
  if (playback) {
    pulse_playback_close(playback);
    free(playback);
  }
}

#endif  // ENABLE_PULSE
