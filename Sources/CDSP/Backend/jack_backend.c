#if defined(ENABLE_JACK)

#include "jack_backend.h"

#include <jack/jack.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Audio/lock_free_ring_buffer.h"
#include "Engine/engine_shared_state.h"
#include "Logging/app_logger.h"

// ============================================================================
// JACK Capture Backend
// ============================================================================

struct jack_capture {
  jack_client_t* client;
  jack_port_t** ports;
  int channels;
  char client_name[256];
  int sample_rate;
  int chunk_size;
  spsc_audio_ring_buffer_t** buffers;
  engine_semaphore_t sem;
  bool active;
  double pending_rate;
  bool rate_changed;
  logger_t logger;
};

/**
 * @brief JACK process callback for capture.
 *
 * Called by JACK server in its real-time thread context.
 * Reads audio from JACK ports and writes to internal ring buffers.
 * Signals the semaphore to notify the reading thread.
 *
 * @param nframes Number of frames to process.
 * @param arg Pointer to jack_capture_t.
 * @return 0 on success.
 */
static int jack_capture_process_cb(jack_nframes_t nframes, void* arg) {
  jack_capture_t* capture = (jack_capture_t*)arg;
  if (!capture->active) return 0;

  for (int c = 0; c < capture->channels; c++) {
    float* in = (float*)jack_port_get_buffer(capture->ports[c], nframes);
    if (in) {
      spsc_audio_ring_buffer_write(capture->buffers[c], in, nframes, 1);
    } else {
      spsc_audio_ring_buffer_write_silence(capture->buffers[c], nframes);
    }
  }

  engine_sem_signal(capture->sem);
  return 0;
}

/**
 * @brief JACK sample rate change callback for capture.
 *
 * Called by JACK server when the sample rate changes.
 *
 * @param nframes New sample rate.
 * @param arg Pointer to jack_capture_t.
 * @return 0.
 */
static int jack_capture_sample_rate_cb(jack_nframes_t nframes, void* arg) {
  jack_capture_t* capture = (jack_capture_t*)arg;
  if ((int)nframes != capture->sample_rate) {
    capture->pending_rate = (double)nframes;
    capture->rate_changed = true;
    logger_warn(&capture->logger, "JACK server sample rate changed to %d Hz",
                log_arg_int((int)nframes), log_arg_none(), log_arg_none(),
                log_arg_none());
  }
  return 0;
}

/**
 * @brief JACK shutdown callback for capture.
 *
 * Called by JACK server on shutdown. Marks backend as inactive and signals
 * semaphore.
 *
 * @param arg Pointer to jack_capture_t.
 */
static void jack_capture_shutdown_cb(void* arg) {
  jack_capture_t* capture = (jack_capture_t*)arg;
  capture->active = false;
  engine_sem_signal(capture->sem);
  logger_error(&capture->logger, "JACK server shutdown", log_arg_none(),
               log_arg_none(), log_arg_none(), log_arg_none());
}

/** @brief Vtable wrapper for jack_capture_destroy. */
static void vtable_capture_destroy(void* ctx) {
  jack_capture_destroy((jack_capture_t*)ctx);
}
/** @brief Vtable wrapper for jack_capture_open. */
static bool vtable_capture_open(void* ctx, backend_error_t* err) {
  return jack_capture_open((jack_capture_t*)ctx, err);
}
/** @brief Vtable wrapper for jack_capture_read. */
static bool vtable_capture_read(void* ctx, size_t frames, audio_chunk_t* chunk,
                                backend_error_t* err) {
  return jack_capture_read((jack_capture_t*)ctx, frames, chunk, err);
}
/** @brief Vtable wrapper for jack_capture_close. */
static void vtable_capture_close(void* ctx) {
  jack_capture_close((jack_capture_t*)ctx);
}
/** @brief Vtable wrapper for jack_capture_get_pending_rate_change. */
static bool vtable_capture_get_pending_rate_change(void* ctx,
                                                   double* out_rate) {
  return jack_capture_get_pending_rate_change((jack_capture_t*)ctx, out_rate);
}
/** @brief Vtable wrapper for jack_capture_pitch_control_supported. */
static bool vtable_capture_pitch_supported(void* ctx) {
  return jack_capture_pitch_control_supported((jack_capture_t*)ctx);
}
/** @brief Vtable wrapper for jack_capture_set_pitch. */
static void vtable_capture_set_pitch(void* ctx, double multiplier) {
  jack_capture_set_pitch((jack_capture_t*)ctx, multiplier);
}
/** @brief Vtable wrapper for jack_capture_wait. */
static bool vtable_capture_wait(void* ctx, uint32_t timeout_ms) {
  return jack_capture_wait((jack_capture_t*)ctx, timeout_ms);
}
/** @brief Vtable wrapper for jack_capture_set_is_paused. */
static void vtable_capture_set_paused(void* ctx, bool paused) {
  (void)ctx;
  (void)paused;
}

static const capture_backend_vtable_t JACK_CAPTURE_VTABLE = {
    .open = vtable_capture_open,
    .read = vtable_capture_read,
    .close = vtable_capture_close,
    .get_pending_rate_change = vtable_capture_get_pending_rate_change,
    .is_pitch_control_supported = vtable_capture_pitch_supported,
    .set_pitch = vtable_capture_set_pitch,
    .wait_for_data = vtable_capture_wait,
    .set_is_paused = vtable_capture_set_paused,
    .destroy = vtable_capture_destroy};

capture_backend_t* jack_capture_create(const capture_device_config_t* config,
                                       int sample_rate, int chunk_size,
                                       processing_parameters_t* params,
                                       backend_error_t* err) {
  (void)params;
  jack_capture_t* capture = (jack_capture_t*)calloc(1, sizeof(jack_capture_t));
  if (!capture) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Memory allocation failed");
    return NULL;
  }

  capture->logger = logger_create("dsp.capture.jack");
  capture->channels = config->cfg.jack.channels;
  capture->sample_rate = sample_rate;
  capture->chunk_size = chunk_size;
  capture->active = false;
  capture->rate_changed = false;
  capture->pending_rate = (double)sample_rate;

  if (strlen(config->cfg.jack.device) > 0) {
    snprintf(capture->client_name, sizeof(capture->client_name), "%s",
             config->cfg.jack.device);
  } else {
    snprintf(capture->client_name, sizeof(capture->client_name),
             "camilladsp_capture");
  }

  if (!engine_sem_init(&capture->sem)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to initialize semaphore");
    free(capture);
    return NULL;
  }

  capture->buffers = (spsc_audio_ring_buffer_t**)calloc(
      capture->channels, sizeof(spsc_audio_ring_buffer_t*));
  size_t buf_capacity = 8 * chunk_size;
  for (int c = 0; c < capture->channels; c++) {
    capture->buffers[c] = spsc_audio_ring_buffer_create(buf_capacity);
  }

  capture_backend_t* backend =
      (capture_backend_t*)malloc(sizeof(capture_backend_t));
  backend->ctx = capture;
  backend->vtable = &JACK_CAPTURE_VTABLE;
  return backend;
}

bool jack_capture_open(jack_capture_t* capture, backend_error_t* err) {
  jack_status_t status;
  capture->client =
      jack_client_open(capture->client_name, JackNullOption, &status);
  if (!capture->client) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to open JACK client");
    return false;
  }

  int server_rate = jack_get_sample_rate(capture->client);
  if (server_rate != capture->sample_rate) {
    if (err) {
      char msg[128];
      snprintf(msg, sizeof(msg),
               "JACK server rate %d Hz does not match configured rate %d Hz",
               server_rate, capture->sample_rate);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    jack_client_close(capture->client);
    capture->client = NULL;
    return false;
  }

  capture->ports =
      (jack_port_t**)calloc(capture->channels, sizeof(jack_port_t*));
  for (int c = 0; c < capture->channels; c++) {
    char name[64];
    snprintf(name, sizeof(name), "in_%d", c + 1);
    capture->ports[c] = jack_port_register(
        capture->client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    if (!capture->ports[c]) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to register JACK port");
      return false;
    }
  }

  jack_set_process_callback(capture->client, jack_capture_process_cb, capture);
  jack_set_sample_rate_callback(capture->client, jack_capture_sample_rate_cb,
                                capture);
  jack_on_shutdown(capture->client, jack_capture_shutdown_cb, capture);

  capture->active = true;
  if (jack_activate(capture->client) != 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to activate JACK client");
    capture->active = false;
    return false;
  }

  return true;
}

bool jack_capture_read(jack_capture_t* capture, size_t frames,
                       audio_chunk_t* chunk, backend_error_t* err) {
  if (!capture->active) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "JACK capture not active");
    return false;
  }

  // Block the calling thread if there is not enough data in the ring buffer.
  // The JACK process callback will signal the semaphore when it writes new
  // data.
  while (spsc_audio_ring_buffer_get_available_to_read(capture->buffers[0]) <
         frames) {
    engine_sem_wait(capture->sem);
    if (!capture->active) return false;
  }

  float* temp = (float*)malloc(frames * sizeof(float));
  for (int c = 0; c < capture->channels; c++) {
    size_t consumed =
        spsc_audio_ring_buffer_consume(capture->buffers[c], temp, frames);
    double* dest = audio_chunk_get_channel(chunk, c);
    for (size_t f = 0; f < consumed; f++) {
      dest[f] = (double)temp[f];
    }
    for (size_t f = consumed; f < frames; f++) {
      dest[f] = 0.0;
    }
  }
  free(temp);

  audio_chunk_set_valid_frames(chunk, frames);
  return true;
}

void jack_capture_close(jack_capture_t* capture) {
  if (!capture) return;
  capture->active = false;
  if (capture->client) {
    jack_deactivate(capture->client);
    jack_client_close(capture->client);
    capture->client = NULL;
  }
  if (capture->ports) {
    free(capture->ports);
    capture->ports = NULL;
  }
}

bool jack_capture_get_pending_rate_change(jack_capture_t* capture,
                                          double* out_rate) {
  if (capture->rate_changed) {
    *out_rate = capture->pending_rate;
    capture->rate_changed = false;
    return true;
  }
  return false;
}

bool jack_capture_pitch_control_supported(jack_capture_t* capture) {
  (void)capture;
  return false;
}

void jack_capture_set_pitch(jack_capture_t* capture, double multiplier) {
  (void)capture;
  (void)multiplier;
}

bool jack_capture_wait(jack_capture_t* capture, uint32_t timeout_ms) {
  (void)timeout_ms;
  if (!capture->active) return false;
  if (spsc_audio_ring_buffer_get_available_to_read(capture->buffers[0]) > 0) {
    return true;
  }
  // Wait on sem
  engine_sem_wait(capture->sem);
  return capture->active;
}

void jack_capture_destroy(jack_capture_t* capture) {
  if (!capture) return;
  jack_capture_close(capture);
  engine_sem_destroy(&capture->sem);
  for (int c = 0; c < capture->channels; c++) {
    spsc_audio_ring_buffer_free(capture->buffers[c]);
  }
  free(capture->buffers);
  free(capture);
}

// ============================================================================
// JACK Playback Backend
// ============================================================================

struct jack_playback {
  jack_client_t* client;
  jack_port_t** ports;
  int channels;
  char client_name[256];
  int sample_rate;
  int chunk_size;
  spsc_audio_ring_buffer_t** buffers;
  engine_semaphore_t sem;
  bool active;
  bool is_paused;
  double pending_rate;
  bool rate_changed;
  logger_t logger;
};

/**
 * @brief JACK process callback for playback.
 *
 * Called by JACK server in its real-time thread context.
 * Consumes audio from internal ring buffers and writes to JACK ports.
 * Signals the semaphore to notify the writing thread.
 *
 * @param nframes Number of frames to process.
 * @param arg Pointer to jack_playback_t.
 * @return 0 on success.
 */
static int jack_playback_process_cb(jack_nframes_t nframes, void* arg) {
  jack_playback_t* playback = (jack_playback_t*)arg;
  if (!playback->active) {
    for (int c = 0; c < playback->channels; c++) {
      float* out = (float*)jack_port_get_buffer(playback->ports[c], nframes);
      if (out) memset(out, 0, nframes * sizeof(float));
    }
    return 0;
  }

  for (int c = 0; c < playback->channels; c++) {
    float* out = (float*)jack_port_get_buffer(playback->ports[c], nframes);
    if (out) {
      size_t consumed =
          spsc_audio_ring_buffer_consume(playback->buffers[c], out, nframes);
      if (consumed < nframes) {
        memset(out + consumed, 0, (nframes - consumed) * sizeof(float));
      }
    }
  }

  engine_sem_signal(playback->sem);
  return 0;
}

/**
 * @brief JACK sample rate change callback for playback.
 *
 * Called by JACK server when the sample rate changes.
 *
 * @param nframes New sample rate.
 * @param arg Pointer to jack_playback_t.
 * @return 0.
 */
static int jack_playback_sample_rate_cb(jack_nframes_t nframes, void* arg) {
  jack_playback_t* playback = (jack_playback_t*)arg;
  if ((int)nframes != playback->sample_rate) {
    playback->pending_rate = (double)nframes;
    playback->rate_changed = true;
    logger_warn(&playback->logger, "JACK server sample rate changed to %d Hz",
                log_arg_int((int)nframes), log_arg_none(), log_arg_none(),
                log_arg_none());
  }
  return 0;
}

/**
 * @brief JACK shutdown callback for playback.
 *
 * Called by JACK server on shutdown. Marks backend as inactive and signals
 * semaphore.
 *
 * @param arg Pointer to jack_playback_t.
 */
static void jack_playback_shutdown_cb(void* arg) {
  jack_playback_t* playback = (jack_playback_t*)arg;
  playback->active = false;
  engine_sem_signal(playback->sem);
  logger_error(&playback->logger, "JACK server shutdown", log_arg_none(),
               log_arg_none(), log_arg_none(), log_arg_none());
}

/** @brief Vtable wrapper for jack_playback_destroy. */
static void vtable_playback_destroy(void* ctx) {
  jack_playback_destroy((jack_playback_t*)ctx);
}
/** @brief Vtable wrapper for jack_playback_open. */
static bool vtable_playback_open(void* ctx, backend_error_t* err) {
  return jack_playback_open((jack_playback_t*)ctx, err);
}
/** @brief Vtable wrapper for jack_playback_write. */
static bool vtable_playback_write(void* ctx, const audio_chunk_t* chunk,
                                  backend_error_t* err) {
  return jack_playback_write((jack_playback_t*)ctx, chunk, err);
}
/** @brief Vtable wrapper for jack_playback_close. */
static void vtable_playback_close(void* ctx) {
  jack_playback_close((jack_playback_t*)ctx);
}
/** @brief Vtable wrapper for jack_playback_get_buffer_level. */
static size_t vtable_playback_get_buffer_level(void* ctx) {
  return jack_playback_get_buffer_level((jack_playback_t*)ctx);
}
/** @brief Vtable wrapper for jack_playback_get_pending_rate_change. */
static bool vtable_playback_get_pending_rate_change(void* ctx,
                                                    double* out_rate) {
  return jack_playback_get_pending_rate_change((jack_playback_t*)ctx, out_rate);
}
/** @brief Vtable wrapper for jack_playback_prefill_silence. */
static bool vtable_playback_prefill_silence(void* ctx, size_t frames,
                                            backend_error_t* err) {
  return jack_playback_prefill_silence((jack_playback_t*)ctx, frames, err);
}
/** @brief Vtable wrapper for jack_playback_get_is_paused. */
static bool vtable_playback_get_is_paused(void* ctx) {
  return jack_playback_get_is_paused((jack_playback_t*)ctx);
}
/** @brief Vtable wrapper for jack_playback_set_is_paused. */
static void vtable_playback_set_is_paused(void* ctx, bool paused) {
  jack_playback_set_is_paused((jack_playback_t*)ctx, paused);
}
/** @brief Vtable wrapper for jack_playback_pitch_control_supported. */
static bool vtable_playback_pitch_supported(void* ctx) {
  (void)ctx;
  return false;
}
/** @brief Vtable wrapper for jack_playback_set_pitch. */
static void vtable_playback_set_pitch(void* ctx, double multiplier) {
  (void)ctx;
  (void)multiplier;
}

static const playback_backend_vtable_t JACK_PLAYBACK_VTABLE = {
    .open = vtable_playback_open,
    .write = vtable_playback_write,
    .close = vtable_playback_close,
    .get_buffer_level = vtable_playback_get_buffer_level,
    .get_pending_rate_change = vtable_playback_get_pending_rate_change,
    .prefill_silence = vtable_playback_prefill_silence,
    .get_is_paused = vtable_playback_get_is_paused,
    .set_is_paused = vtable_playback_set_is_paused,
    .pitch_control_supported = vtable_playback_pitch_supported,
    .set_pitch = vtable_playback_set_pitch,
    .destroy = vtable_playback_destroy};

playback_backend_t* jack_playback_create(const playback_device_config_t* config,
                                         int sample_rate, int chunk_size,
                                         processing_parameters_t* params,
                                         backend_error_t* err) {
  (void)params;
  jack_playback_t* playback =
      (jack_playback_t*)calloc(1, sizeof(jack_playback_t));
  if (!playback) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Memory allocation failed");
    return NULL;
  }

  playback->logger = logger_create("dsp.playback.jack");
  playback->channels = config->cfg.jack.channels;
  playback->sample_rate = sample_rate;
  playback->chunk_size = chunk_size;
  playback->active = false;
  playback->is_paused = false;
  playback->rate_changed = false;
  playback->pending_rate = (double)sample_rate;

  if (strlen(config->cfg.jack.device) > 0) {
    snprintf(playback->client_name, sizeof(playback->client_name), "%s",
             config->cfg.jack.device);
  } else {
    snprintf(playback->client_name, sizeof(playback->client_name),
             "camilladsp_playback");
  }

  if (!engine_sem_init(&playback->sem)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to initialize semaphore");
    free(playback);
    return NULL;
  }

  playback->buffers = (spsc_audio_ring_buffer_t**)calloc(
      playback->channels, sizeof(spsc_audio_ring_buffer_t*));
  size_t buf_capacity = 8 * chunk_size;
  for (int c = 0; c < playback->channels; c++) {
    playback->buffers[c] = spsc_audio_ring_buffer_create(buf_capacity);
  }

  playback_backend_t* backend =
      (playback_backend_t*)malloc(sizeof(playback_backend_t));
  backend->ctx = playback;
  backend->vtable = &JACK_PLAYBACK_VTABLE;
  return backend;
}

bool jack_playback_open(jack_playback_t* playback, backend_error_t* err) {
  jack_status_t status;
  playback->client =
      jack_client_open(playback->client_name, JackNullOption, &status);
  if (!playback->client) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to open JACK client");
    return false;
  }

  int server_rate = jack_get_sample_rate(playback->client);
  if (server_rate != playback->sample_rate) {
    if (err) {
      char msg[128];
      snprintf(msg, sizeof(msg),
               "JACK server rate %d Hz does not match configured rate %d Hz",
               server_rate, playback->sample_rate);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    jack_client_close(playback->client);
    playback->client = NULL;
    return false;
  }

  playback->ports =
      (jack_port_t**)calloc(playback->channels, sizeof(jack_port_t*));
  for (int c = 0; c < playback->channels; c++) {
    char name[64];
    snprintf(name, sizeof(name), "out_%d", c + 1);
    playback->ports[c] = jack_port_register(
        playback->client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    if (!playback->ports[c]) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to register JACK port");
      return false;
    }
  }

  jack_set_process_callback(playback->client, jack_playback_process_cb,
                            playback);
  jack_set_sample_rate_callback(playback->client, jack_playback_sample_rate_cb,
                                playback);
  jack_on_shutdown(playback->client, jack_playback_shutdown_cb, playback);

  playback->active = true;
  if (jack_activate(playback->client) != 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to activate JACK client");
    playback->active = false;
    return false;
  }

  return true;
}

bool jack_playback_write(jack_playback_t* playback, const audio_chunk_t* chunk,
                         backend_error_t* err) {
  if (!playback->active) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "JACK playback not active");
    return false;
  }

  size_t frames = audio_chunk_get_valid_frames(chunk);

  // Block the calling thread if there is not enough space in the ring buffer.
  // The JACK process callback will signal the semaphore when it consumes data.
  while (spsc_audio_ring_buffer_get_available_to_write(playback->buffers[0]) <
         frames) {
    engine_sem_wait(playback->sem);
    if (!playback->active) return false;
  }

  float* temp = (float*)malloc(frames * sizeof(float));
  for (int c = 0; c < playback->channels; c++) {
    const double* src = audio_chunk_get_channel(chunk, c);
    for (size_t f = 0; f < frames; f++) {
      temp[f] = (float)src[f];
    }
    spsc_audio_ring_buffer_write(playback->buffers[c], temp, frames, 1);
  }
  free(temp);

  return true;
}

void jack_playback_close(jack_playback_t* playback) {
  if (!playback) return;
  playback->active = false;
  if (playback->client) {
    jack_deactivate(playback->client);
    jack_client_close(playback->client);
    playback->client = NULL;
  }
  if (playback->ports) {
    free(playback->ports);
    playback->ports = NULL;
  }
}

size_t jack_playback_get_buffer_level(jack_playback_t* playback) {
  if (!playback->active) return 0;
  return spsc_audio_ring_buffer_get_available_to_read(playback->buffers[0]);
}

bool jack_playback_get_pending_rate_change(jack_playback_t* playback,
                                           double* out_rate) {
  if (playback->rate_changed) {
    *out_rate = playback->pending_rate;
    playback->rate_changed = false;
    return true;
  }
  return false;
}

bool jack_playback_prefill_silence(jack_playback_t* playback, size_t frames,
                                   backend_error_t* err) {
  (void)err;
  if (!playback->active) return false;
  for (int c = 0; c < playback->channels; c++) {
    spsc_audio_ring_buffer_write_silence(playback->buffers[c], frames);
  }
  return true;
}

bool jack_playback_get_is_paused(jack_playback_t* playback) {
  return playback->is_paused;
}

void jack_playback_set_is_paused(jack_playback_t* playback, bool paused) {
  playback->is_paused = paused;
}

void jack_playback_destroy(jack_playback_t* playback) {
  if (!playback) return;
  jack_playback_close(playback);
  engine_sem_destroy(&playback->sem);
  for (int c = 0; c < playback->channels; c++) {
    spsc_audio_ring_buffer_free(playback->buffers[c]);
  }
  free(playback->buffers);
  free(playback);
}

#endif  // ENABLE_JACK
