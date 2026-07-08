#if defined(ENABLE_PIPEWIRE)

#include "pipewire_backend.h"

#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <spa/param/audio/format-utils.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "Audio/lock_free_ring_buffer.h"
#include "Logging/app_logger.h"

struct pipewire_capture {
  char device[256];
  int sample_rate;
  int channels;
  int chunk_size;

  char node_name[256];
  char node_description[256];
  char node_group_name[256];
  char autoconnect_to[256];
  bool has_node_name;
  bool has_node_description;
  bool has_node_group_name;
  bool has_autoconnect_to;

  struct pw_thread_loop* loop;
  struct pw_context* context;
  struct pw_stream* stream;

  spsc_audio_ring_buffer_t* ring;
  float* decode_buf;
  size_t decode_buf_size;
};

struct pipewire_playback {
  char device[256];
  int sample_rate;
  int channels;
  int chunk_size;

  char node_name[256];
  char node_description[256];
  char node_group_name[256];
  char autoconnect_to[256];
  bool has_node_name;
  bool has_node_description;
  bool has_node_group_name;
  bool has_autoconnect_to;

  struct pw_thread_loop* loop;
  struct pw_context* context;
  struct pw_stream* stream;

  spsc_audio_ring_buffer_t* ring;
  float* encode_buf;
  size_t encode_buf_size;
  bool paused;
};

// MARK: - PipeWire Callbacks

static void on_capture_process(void* data) {
  pipewire_capture_t* c = (pipewire_capture_t*)data;
  struct pw_buffer* b = pw_stream_dequeue_buffer(c->stream);
  if (!b) return;

  struct spa_buffer* buf = b->buffer;
  const float* src = (const float*)buf->datas[0].data;
  if (src) {
    size_t offset = buf->datas[0].chunk->offset;
    size_t size = buf->datas[0].chunk->size;
    size_t frames = size / (sizeof(float) * c->channels);

    spsc_audio_ring_buffer_write(c->ring, src + (offset / sizeof(float)),
                                 frames * c->channels, 1);
  }

  pw_stream_queue_buffer(c->stream, b);
}

static const struct pw_stream_events capture_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_capture_process,
};

static void on_playback_process(void* data) {
  pipewire_playback_t* p = (pipewire_playback_t*)data;
  struct pw_buffer* b = pw_stream_dequeue_buffer(p->stream);
  if (!b) return;

  struct spa_buffer* buf = b->buffer;
  float* dst = (float*)buf->datas[0].data;
  if (dst) {
    size_t max_bytes = buf->datas[0].maxsize;
    size_t frame_size = sizeof(float) * p->channels;
    size_t max_frames = max_bytes / frame_size;

    size_t requested = max_frames * p->channels;
    size_t consumed = spsc_audio_ring_buffer_consume(p->ring, dst, requested);

    if (consumed < requested) {
      // Fill remaining with silence
      memset(dst + consumed, 0, (requested - consumed) * sizeof(float));
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->size = max_frames * frame_size;
    buf->datas[0].chunk->stride = frame_size;
  }

  pw_stream_queue_buffer(p->stream, b);
}

static const struct pw_stream_events playback_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_playback_process,
};

// MARK: - Capture Backend implementation

static bool cap_vtable_open(void* ctx, backend_error_t* err) {
  return pipewire_capture_open((pipewire_capture_t*)ctx, err);
}
static bool cap_vtable_read(void* ctx, size_t frames, audio_chunk_t* chunk,
                            backend_error_t* err) {
  return pipewire_capture_read((pipewire_capture_t*)ctx, frames, chunk, err);
}
static void cap_vtable_close(void* ctx) {
  pipewire_capture_close((pipewire_capture_t*)ctx);
}
static bool cap_vtable_get_pending_rate_change(void* ctx, double* out_rate) {
  return pipewire_capture_get_pending_rate_change((pipewire_capture_t*)ctx,
                                                  out_rate);
}
static bool cap_vtable_is_pitch_control_supported(void* ctx) {
  return pipewire_capture_pitch_control_supported((pipewire_capture_t*)ctx);
}
static void cap_vtable_set_pitch(void* ctx, double multiplier) {
  pipewire_capture_set_pitch((pipewire_capture_t*)ctx, multiplier);
}
static bool cap_vtable_wait_for_data(void* ctx, uint32_t timeout_ms) {
  return pipewire_capture_wait((pipewire_capture_t*)ctx, timeout_ms);
}
static void cap_vtable_destroy(void* ctx) {
  pipewire_capture_destroy((pipewire_capture_t*)ctx);
}

static const capture_backend_vtable_t pipewire_capture_vtable = {
    .open = cap_vtable_open,
    .read = cap_vtable_read,
    .close = cap_vtable_close,
    .get_pending_rate_change = cap_vtable_get_pending_rate_change,
    .is_pitch_control_supported = cap_vtable_is_pitch_control_supported,
    .set_pitch = cap_vtable_set_pitch,
    .wait_for_data = cap_vtable_wait_for_data,
    .destroy = cap_vtable_destroy};

capture_backend_t* pipewire_capture_create(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    processing_parameters_t* params, backend_error_t* err) {
  (void)params;
  (void)err;
  pipewire_capture_t* capture =
      (pipewire_capture_t*)calloc(1, sizeof(pipewire_capture_t));
  if (!capture) return NULL;

  if (config->cfg.pipewire.has_device &&
      strcmp(config->cfg.pipewire.device, "default") != 0) {
    snprintf(capture->device, sizeof(capture->device), "%s",
             config->cfg.pipewire.device);
  } else {
    capture->device[0] = '\0';
  }

  capture->sample_rate = sample_rate;
  capture->channels = config->cfg.pipewire.channels;
  capture->chunk_size = chunk_size;

  if (config->cfg.pipewire.has_node_name) {
    strncpy(capture->node_name, config->cfg.pipewire.node_name,
            sizeof(capture->node_name) - 1);
    capture->has_node_name = true;
  }
  if (config->cfg.pipewire.has_node_description) {
    strncpy(capture->node_description, config->cfg.pipewire.node_description,
            sizeof(capture->node_description) - 1);
    capture->has_node_description = true;
  }
  if (config->cfg.pipewire.has_node_group_name) {
    strncpy(capture->node_group_name, config->cfg.pipewire.node_group_name,
            sizeof(capture->node_group_name) - 1);
    capture->has_node_group_name = true;
  }
  if (config->cfg.pipewire.has_autoconnect_to) {
    strncpy(capture->autoconnect_to, config->cfg.pipewire.autoconnect_to,
            sizeof(capture->autoconnect_to) - 1);
    capture->has_autoconnect_to = true;
  }

  capture_backend_t* backend =
      (capture_backend_t*)calloc(1, sizeof(capture_backend_t));
  if (!backend) {
    free(capture);
    return NULL;
  }
  backend->ctx = capture;
  backend->vtable = &pipewire_capture_vtable;
  return backend;
}

bool pipewire_capture_open(pipewire_capture_t* capture, backend_error_t* err) {
  pw_init(NULL, NULL);

  capture->loop = pw_thread_loop_new("CDSP-Capture-Loop", NULL);
  if (!capture->loop) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create PipeWire thread loop");
    return false;
  }

  if (pw_thread_loop_start(capture->loop) < 0) {
    pw_thread_loop_destroy(capture->loop);
    capture->loop = NULL;
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to start PipeWire thread loop");
    return false;
  }

  pw_thread_loop_lock(capture->loop);

  capture->context =
      pw_context_new(pw_thread_loop_get_loop(capture->loop), NULL, 0);
  if (!capture->context) {
    pw_thread_loop_unlock(capture->loop);
    pw_thread_loop_stop(capture->loop);
    pw_thread_loop_destroy(capture->loop);
    capture->loop = NULL;
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create PipeWire context");
    return false;
  }

  const char* node_name =
      capture->has_node_name ? capture->node_name : "cdsp-capture";
  const char* node_desc = capture->has_node_description
                              ? capture->node_description
                              : "CDSP Capture";
  const char* node_group =
      capture->has_node_group_name ? capture->node_group_name : "cdsp";

  struct pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE, "DSP", PW_KEY_APP_NAME, "CDSP", PW_KEY_NODE_NAME,
      node_name, PW_KEY_NODE_DESCRIPTION, node_desc, PW_KEY_NODE_GROUP,
      node_group, NULL);

  if (props) {
    char latency_str[64];
    snprintf(latency_str, sizeof(latency_str), "%d/%d", capture->chunk_size,
             capture->sample_rate);
    pw_properties_set(props, PW_KEY_NODE_LATENCY, latency_str);
    if (capture->device[0] != '\0') {
      pw_properties_set(props, "target.object", capture->device);
    } else if (capture->has_autoconnect_to) {
      pw_properties_set(props, "target.object", capture->autoconnect_to);
    }
  }

  capture->stream = pw_stream_new_simple(pw_thread_loop_get_loop(capture->loop),
                                         "CDSP-Capture-Stream", props,
                                         &capture_stream_events, capture);

  if (!capture->stream) {
    pw_context_destroy(capture->context);
    pw_thread_loop_unlock(capture->loop);
    pw_thread_loop_stop(capture->loop);
    pw_thread_loop_destroy(capture->loop);
    capture->loop = NULL;
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create PipeWire stream");
    return false;
  }

  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const struct spa_pod* params[1];
  struct spa_audio_info_raw info = {.format = SPA_AUDIO_FORMAT_F32_LE,
                                    .rate = (uint32_t)capture->sample_rate,
                                    .channels = (uint32_t)capture->channels};
  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

  uint32_t flags = PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
  if (capture->device[0] != '\0' || capture->has_autoconnect_to) {
    flags |= PW_STREAM_FLAG_AUTOCONNECT;
  }

  int rc = pw_stream_connect(capture->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                             flags, params, 1);

  pw_thread_loop_unlock(capture->loop);

  if (rc < 0) {
    pw_thread_loop_lock(capture->loop);
    pw_stream_destroy(capture->stream);
    pw_context_destroy(capture->context);
    pw_thread_loop_unlock(capture->loop);
    pw_thread_loop_stop(capture->loop);
    pw_thread_loop_destroy(capture->loop);
    capture->loop = NULL;
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to connect PipeWire stream");
    return false;
  }

  // Create large SPSC ring buffer (8 blocks headroom)
  capture->ring = spsc_audio_ring_buffer_create(capture->chunk_size *
                                                capture->channels * 8);
  capture->decode_buf_size = capture->chunk_size * capture->channels;
  capture->decode_buf =
      (float*)malloc(capture->decode_buf_size * sizeof(float));

  logger_t logger = logger_create("dsp.backend.pipewire");
  logger_info(
      &logger, "Opened PipeWire capture: device=%s, rate=%d, channels=%d",
      log_arg_string(capture->device[0] != '\0' ? capture->device : "default"),
      log_arg_int((int64_t)capture->sample_rate),
      log_arg_int((int64_t)capture->channels), log_arg_none());

  return true;
}

bool pipewire_capture_read(pipewire_capture_t* capture, size_t frames,
                           audio_chunk_t* chunk, backend_error_t* err) {
  (void)err;
  size_t requested = frames * capture->channels;
  if (requested > capture->decode_buf_size) {
    capture->decode_buf =
        (float*)realloc(capture->decode_buf, requested * sizeof(float));
    capture->decode_buf_size = requested;
  }

  if (spsc_audio_ring_buffer_get_available_to_read(capture->ring) < requested) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_NONE, "");
    }
    return false;
  }

  size_t consumed = spsc_audio_ring_buffer_consume(
      capture->ring, capture->decode_buf, requested);
  if (consumed < requested) {
    memset(capture->decode_buf + consumed, 0,
           (requested - consumed) * sizeof(float));
  }

  for (size_t f = 0; f < frames; f++) {
    for (int c = 0; c < capture->channels; c++) {
      audio_chunk_get_channel(chunk, c)[f] =
          (double)capture->decode_buf[f * capture->channels + c];
    }
  }

  audio_chunk_set_valid_frames(chunk, frames);
  return true;
}

void pipewire_capture_close(pipewire_capture_t* capture) {
  if (capture->loop) {
    pw_thread_loop_lock(capture->loop);
    if (capture->stream) {
      pw_stream_destroy(capture->stream);
      capture->stream = NULL;
    }
    if (capture->context) {
      pw_context_destroy(capture->context);
      capture->context = NULL;
    }
    pw_thread_loop_unlock(capture->loop);

    pw_thread_loop_stop(capture->loop);
    pw_thread_loop_destroy(capture->loop);
    capture->loop = NULL;
  }

  if (capture->ring) {
    spsc_audio_ring_buffer_free(capture->ring);
    capture->ring = NULL;
  }
  if (capture->decode_buf) {
    free(capture->decode_buf);
    capture->decode_buf = NULL;
  }
}

bool pipewire_capture_get_pending_rate_change(pipewire_capture_t* capture,
                                              double* out_rate) {
  (void)capture;
  (void)out_rate;
  return false;
}

bool pipewire_capture_pitch_control_supported(pipewire_capture_t* capture) {
  (void)capture;
  return false;
}

void pipewire_capture_set_pitch(pipewire_capture_t* capture,
                                double multiplier) {
  (void)capture;
  (void)multiplier;
}

bool pipewire_capture_wait(pipewire_capture_t* capture, uint32_t timeout_ms) {
  if (!capture->ring) return false;
  size_t requested = capture->chunk_size * capture->channels;
  uint32_t elapsed = 0;
  while (spsc_audio_ring_buffer_get_available_to_read(capture->ring) <
         requested) {
    if (elapsed >= timeout_ms) {
      return false;
    }
    struct timespec req = {.tv_sec = 0, .tv_nsec = 1000000L};
    nanosleep(&req, NULL);
    elapsed += 1;
  }
  return true;
}

void pipewire_capture_destroy(pipewire_capture_t* capture) { free(capture); }

// MARK: - Playback Backend implementation

static bool play_vtable_open(void* ctx, backend_error_t* err) {
  return pipewire_playback_open((pipewire_playback_t*)ctx, err);
}
static bool play_vtable_write(void* ctx, const audio_chunk_t* chunk,
                              backend_error_t* err) {
  return pipewire_playback_write((pipewire_playback_t*)ctx, chunk, err);
}
static void play_vtable_close(void* ctx) {
  pipewire_playback_close((pipewire_playback_t*)ctx);
}
static size_t play_vtable_get_buffer_level(void* ctx) {
  return pipewire_playback_get_buffer_level((pipewire_playback_t*)ctx);
}
static bool play_vtable_get_pending_rate_change(void* ctx, double* out_rate) {
  return pipewire_playback_get_pending_rate_change((pipewire_playback_t*)ctx,
                                                   out_rate);
}
static bool play_vtable_prefill_silence(void* ctx, size_t frames,
                                        backend_error_t* err) {
  return pipewire_playback_prefill_silence((pipewire_playback_t*)ctx, frames,
                                           err);
}
static bool play_vtable_get_is_paused(void* ctx) {
  return pipewire_playback_get_is_paused((pipewire_playback_t*)ctx);
}
static void play_vtable_set_is_paused(void* ctx, bool paused) {
  pipewire_playback_set_is_paused((pipewire_playback_t*)ctx, paused);
}
static void play_vtable_destroy(void* ctx) {
  pipewire_playback_destroy((pipewire_playback_t*)ctx);
}

static const playback_backend_vtable_t pipewire_playback_vtable = {
    .open = play_vtable_open,
    .write = play_vtable_write,
    .close = play_vtable_close,
    .get_buffer_level = play_vtable_get_buffer_level,
    .get_pending_rate_change = play_vtable_get_pending_rate_change,
    .prefill_silence = play_vtable_prefill_silence,
    .get_is_paused = play_vtable_get_is_paused,
    .set_is_paused = play_vtable_set_is_paused,
    .destroy = play_vtable_destroy};

playback_backend_t* pipewire_playback_create(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    processing_parameters_t* params, backend_error_t* err) {
  (void)params;
  (void)err;
  pipewire_playback_t* playback =
      (pipewire_playback_t*)calloc(1, sizeof(pipewire_playback_t));
  if (!playback) return NULL;

  if (config->cfg.pipewire.has_device &&
      strcmp(config->cfg.pipewire.device, "default") != 0) {
    snprintf(playback->device, sizeof(playback->device), "%s",
             config->cfg.pipewire.device);
  } else {
    playback->device[0] = '\0';
  }

  playback->sample_rate = sample_rate;
  playback->channels = config->cfg.pipewire.channels;
  playback->chunk_size = chunk_size;

  if (config->cfg.pipewire.has_node_name) {
    strncpy(playback->node_name, config->cfg.pipewire.node_name,
            sizeof(playback->node_name) - 1);
    playback->has_node_name = true;
  }
  if (config->cfg.pipewire.has_node_description) {
    strncpy(playback->node_description, config->cfg.pipewire.node_description,
            sizeof(playback->node_description) - 1);
    playback->has_node_description = true;
  }
  if (config->cfg.pipewire.has_node_group_name) {
    strncpy(playback->node_group_name, config->cfg.pipewire.node_group_name,
            sizeof(playback->node_group_name) - 1);
    playback->has_node_group_name = true;
  }
  if (config->cfg.pipewire.has_autoconnect_to) {
    strncpy(playback->autoconnect_to, config->cfg.pipewire.autoconnect_to,
            sizeof(playback->autoconnect_to) - 1);
    playback->has_autoconnect_to = true;
  }

  playback_backend_t* backend =
      (playback_backend_t*)calloc(1, sizeof(playback_backend_t));
  if (!backend) {
    free(playback);
    return NULL;
  }
  backend->ctx = playback;
  backend->vtable = &pipewire_playback_vtable;
  return backend;
}

bool pipewire_playback_open(pipewire_playback_t* playback,
                            backend_error_t* err) {
  pw_init(NULL, NULL);

  playback->loop = pw_thread_loop_new("CDSP-Playback-Loop", NULL);
  if (!playback->loop) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create PipeWire thread loop");
    return false;
  }

  if (pw_thread_loop_start(playback->loop) < 0) {
    pw_thread_loop_destroy(playback->loop);
    playback->loop = NULL;
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to start PipeWire thread loop");
    return false;
  }

  pw_thread_loop_lock(playback->loop);

  playback->context =
      pw_context_new(pw_thread_loop_get_loop(playback->loop), NULL, 0);
  if (!playback->context) {
    pw_thread_loop_unlock(playback->loop);
    pw_thread_loop_stop(playback->loop);
    pw_thread_loop_destroy(playback->loop);
    playback->loop = NULL;
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create PipeWire context");
    return false;
  }

  const char* node_name =
      playback->has_node_name ? playback->node_name : "cdsp-playback";
  const char* node_desc = playback->has_node_description
                              ? playback->node_description
                              : "CDSP Playback";
  const char* node_group =
      playback->has_node_group_name ? playback->node_group_name : "cdsp";

  struct pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
      PW_KEY_MEDIA_ROLE, "DSP", PW_KEY_APP_NAME, "CDSP", PW_KEY_NODE_NAME,
      node_name, PW_KEY_NODE_DESCRIPTION, node_desc, PW_KEY_NODE_GROUP,
      node_group, NULL);

  if (props) {
    char latency_str[64];
    snprintf(latency_str, sizeof(latency_str), "%d/%d", playback->chunk_size,
             playback->sample_rate);
    pw_properties_set(props, PW_KEY_NODE_LATENCY, latency_str);
    if (playback->device[0] != '\0') {
      pw_properties_set(props, "target.object", playback->device);
    } else if (playback->has_autoconnect_to) {
      pw_properties_set(props, "target.object", playback->autoconnect_to);
    }
  }

  playback->stream = pw_stream_new_simple(
      pw_thread_loop_get_loop(playback->loop), "CDSP-Playback-Stream", props,
      &playback_stream_events, playback);

  if (!playback->stream) {
    pw_context_destroy(playback->context);
    pw_thread_loop_unlock(playback->loop);
    pw_thread_loop_stop(playback->loop);
    pw_thread_loop_destroy(playback->loop);
    playback->loop = NULL;
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create PipeWire stream");
    return false;
  }

  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const struct spa_pod* params[1];
  struct spa_audio_info_raw info = {.format = SPA_AUDIO_FORMAT_F32_LE,
                                    .rate = (uint32_t)playback->sample_rate,
                                    .channels = (uint32_t)playback->channels};
  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

  uint32_t flags = PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
  if (playback->device[0] != '\0' || playback->has_autoconnect_to) {
    flags |= PW_STREAM_FLAG_AUTOCONNECT;
  }

  int rc = pw_stream_connect(playback->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                             flags, params, 1);

  pw_thread_loop_unlock(playback->loop);

  if (rc < 0) {
    pw_thread_loop_lock(playback->loop);
    pw_stream_destroy(playback->stream);
    pw_context_destroy(playback->context);
    pw_thread_loop_unlock(playback->loop);
    pw_thread_loop_stop(playback->loop);
    pw_thread_loop_destroy(playback->loop);
    playback->loop = NULL;
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to connect PipeWire stream");
    return false;
  }

  playback->ring = spsc_audio_ring_buffer_create(playback->chunk_size *
                                                 playback->channels * 8);
  playback->encode_buf_size = playback->chunk_size * playback->channels;
  playback->encode_buf =
      (float*)malloc(playback->encode_buf_size * sizeof(float));
  playback->paused = false;

  logger_t logger = logger_create("dsp.backend.pipewire");
  logger_info(&logger,
              "Opened PipeWire playback: device=%s, rate=%d, channels=%d",
              log_arg_string(playback->device[0] != '\0' ? playback->device
                                                         : "default"),
              log_arg_int((int64_t)playback->sample_rate),
              log_arg_int((int64_t)playback->channels), log_arg_none());

  return true;
}

bool pipewire_playback_write(pipewire_playback_t* playback,
                             const audio_chunk_t* chunk, backend_error_t* err) {
  if (playback->paused) return true;
  (void)err;

  size_t frames = audio_chunk_get_valid_frames(chunk);
  size_t requested = frames * playback->channels;
  if (requested > playback->encode_buf_size) {
    playback->encode_buf =
        (float*)realloc(playback->encode_buf, requested * sizeof(float));
    playback->encode_buf_size = requested;
  }

  for (size_t f = 0; f < frames; f++) {
    for (int c = 0; c < playback->channels; c++) {
      playback->encode_buf[f * playback->channels + c] =
          (float)audio_chunk_get_channel(chunk, c)[f];
    }
  }

  // Wait until there is space in the SPSC ring buffer (to prevent overwriting
  // oldest data)
  int retries = 100;
  while (spsc_audio_ring_buffer_get_available_to_read(playback->ring) +
             requested >
         spsc_audio_ring_buffer_get_capacity(playback->ring)) {
    if (retries-- <= 0) {
      return false;
    }
    struct timespec req = {.tv_sec = 0, .tv_nsec = 1000000L};  // 1ms
    nanosleep(&req, NULL);
  }

  spsc_audio_ring_buffer_write(playback->ring, playback->encode_buf, requested,
                               1);
  return true;
}

void pipewire_playback_close(pipewire_playback_t* playback) {
  if (playback->loop) {
    // Wait for ring buffer to drain
    int retries = 200;  // wait up to 200ms
    while (spsc_audio_ring_buffer_get_available_to_read(playback->ring) > 0 &&
           retries-- > 0) {
      struct timespec req = {.tv_sec = 0, .tv_nsec = 1000000L};
      nanosleep(&req, NULL);
    }

    pw_thread_loop_lock(playback->loop);
    if (playback->stream) {
      pw_stream_destroy(playback->stream);
      playback->stream = NULL;
    }
    if (playback->context) {
      pw_context_destroy(playback->context);
      playback->context = NULL;
    }
    pw_thread_loop_unlock(playback->loop);

    pw_thread_loop_stop(playback->loop);
    pw_thread_loop_destroy(playback->loop);
    playback->loop = NULL;
  }

  if (playback->ring) {
    spsc_audio_ring_buffer_free(playback->ring);
    playback->ring = NULL;
  }
  if (playback->encode_buf) {
    free(playback->encode_buf);
    playback->encode_buf = NULL;
  }
}

size_t pipewire_playback_get_buffer_level(pipewire_playback_t* playback) {
  if (!playback->ring) return 0;
  return spsc_audio_ring_buffer_get_available_to_read(playback->ring) /
         playback->channels;
}

bool pipewire_playback_get_pending_rate_change(pipewire_playback_t* playback,
                                               double* out_rate) {
  (void)playback;
  (void)out_rate;
  return false;
}

bool pipewire_playback_prefill_silence(pipewire_playback_t* playback,
                                       size_t frames, backend_error_t* err) {
  (void)err;
  if (!playback->ring) return false;

  spsc_audio_ring_buffer_write_silence(playback->ring,
                                       frames * playback->channels);
  return true;
}

bool pipewire_playback_get_is_paused(pipewire_playback_t* playback) {
  return playback->paused;
}

void pipewire_playback_set_is_paused(pipewire_playback_t* playback,
                                     bool paused) {
  playback->paused = paused;
}

void pipewire_playback_destroy(pipewire_playback_t* playback) {
  free(playback);
}

#endif  // ENABLE_PIPEWIRE
