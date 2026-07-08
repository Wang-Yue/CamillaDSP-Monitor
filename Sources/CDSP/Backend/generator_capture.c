#include "generator_capture.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "Logging/app_logger.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if defined(_WIN32)
static inline int rand_r(unsigned int* seed) {
  *seed = *seed * 1103515245 + 12345;
  return (unsigned int)(*seed / 65536) % 32768;
}
#endif

struct generator_capture {
  signal_type_t signal_type;
  double frequency;
  double amplitude;
  int sample_rate;
  int channels;
  int chunk_size;
  double phase;
  unsigned int rand_seed;
  uint64_t last_read_time_ns;
  bool is_paused;
};

static uint64_t get_time_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static double db_to_linear(double db) { return pow(10.0, db / 20.0); }

static bool vtable_open(void* ctx, backend_error_t* err) {
  return generator_capture_open((generator_capture_t*)ctx, err);
}

static bool vtable_read(void* ctx, size_t frames, audio_chunk_t* chunk,
                        backend_error_t* err) {
  return generator_capture_read((generator_capture_t*)ctx, frames, chunk, err);
}

static void vtable_close(void* ctx) {
  generator_capture_close((generator_capture_t*)ctx);
}

static bool vtable_get_pending_rate_change(void* ctx, double* out_rate) {
  return generator_capture_get_pending_rate_change((generator_capture_t*)ctx,
                                                   out_rate);
}

static bool vtable_is_pitch_control_supported(void* ctx) {
  return generator_capture_pitch_control_supported((generator_capture_t*)ctx);
}

static void vtable_set_pitch(void* ctx, double multiplier) {
  generator_capture_set_pitch((generator_capture_t*)ctx, multiplier);
}

static bool vtable_wait_for_data(void* ctx, uint32_t timeout_ms) {
  return generator_capture_wait((generator_capture_t*)ctx, timeout_ms);
}

static void vtable_destroy(void* ctx) {
  generator_capture_destroy((generator_capture_t*)ctx);
}
static void vtable_set_is_paused(void* ctx, bool paused) {
  generator_capture_set_is_paused((generator_capture_t*)ctx, paused);
}

static const capture_backend_vtable_t generator_capture_vtable = {
    .open = vtable_open,
    .read = vtable_read,
    .close = vtable_close,
    .get_pending_rate_change = vtable_get_pending_rate_change,
    .is_pitch_control_supported = vtable_is_pitch_control_supported,
    .set_pitch = vtable_set_pitch,
    .wait_for_data = vtable_wait_for_data,
    .set_is_paused = vtable_set_is_paused,
    .destroy = vtable_destroy};

capture_backend_t* generator_capture_create(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    processing_parameters_t* params, backend_error_t* err) {
  (void)params;
  (void)err;

  generator_capture_t* capture =
      (generator_capture_t*)calloc(1, sizeof(generator_capture_t));
  if (!capture) return NULL;

  if (config->has_generator) {
    capture->signal_type = config->generator.type;
    capture->frequency = config->generator.frequency;
    capture->amplitude = db_to_linear(config->generator.level);
  } else {
    capture->signal_type = SIGNAL_TYPE_SINE;
    capture->frequency = 1000.0;
    capture->amplitude = db_to_linear(0.0);
  }

  capture->sample_rate = sample_rate;
  capture->channels = config->channels;
  capture->chunk_size = chunk_size;
  capture->rand_seed = (unsigned int)(get_time_ns() & 0xFFFFFFFF);

  capture_backend_t* backend =
      (capture_backend_t*)calloc(1, sizeof(capture_backend_t));
  if (!backend) {
    free(capture);
    return NULL;
  }

  backend->ctx = capture;
  backend->vtable = &generator_capture_vtable;
  return backend;
}

bool generator_capture_open(generator_capture_t* capture,
                            backend_error_t* err) {
  (void)err;
  capture->last_read_time_ns = get_time_ns();
  capture->phase = 0.0;

  logger_t logger = logger_create("dsp.backend.generator");
  logger_info(&logger,
              "Opened generator capture: type=%s, freq=%.1f Hz, amp=%.3f",
              log_arg_string(signal_type_to_string(capture->signal_type)),
              log_arg_double(capture->frequency),
              log_arg_double(capture->amplitude), log_arg_none());
  return true;
}

bool generator_capture_read(generator_capture_t* capture, size_t frames,
                            audio_chunk_t* chunk, backend_error_t* err) {
  (void)err;
  if (capture->is_paused) {
    chunk->valid_frames = 0;
    return false;
  }

  double freq_delta = capture->frequency / (double)capture->sample_rate;

  for (size_t f = 0; f < frames; f++) {
    double val = 0.0;
    switch (capture->signal_type) {
      case SIGNAL_TYPE_SINE:
        val = sin(capture->phase * 2.0 * M_PI) * capture->amplitude;
        break;
      case SIGNAL_TYPE_SQUARE:
        val = (sin(capture->phase * 2.0 * M_PI) >= 0.0 ? 1.0 : -1.0) *
              capture->amplitude;
        break;
      case SIGNAL_TYPE_WHITE_NOISE:
        val = (((double)rand_r(&capture->rand_seed) / (double)RAND_MAX) * 2.0 -
               1.0) *
              capture->amplitude;
        break;
      default:
        val = 0.0;
        break;
    }

    for (int c = 0; c < capture->channels; c++) {
      audio_chunk_get_channel(chunk, c)[f] = val;
    }

    capture->phase += freq_delta;
    if (capture->phase >= 1.0) {
      capture->phase -= 1.0;
    }
  }

  chunk->valid_frames = frames;
  return true;
}

void generator_capture_close(generator_capture_t* capture) { (void)capture; }

bool generator_capture_get_pending_rate_change(generator_capture_t* capture,
                                               double* out_rate) {
  (void)capture;
  (void)out_rate;
  return false;
}

bool generator_capture_pitch_control_supported(generator_capture_t* capture) {
  (void)capture;
  return false;
}

void generator_capture_set_pitch(generator_capture_t* capture,
                                 double multiplier) {
  (void)capture;
  (void)multiplier;
}

bool generator_capture_wait(generator_capture_t* capture, uint32_t timeout_ms) {
  (void)capture;
  struct timespec req = {.tv_sec = (time_t)(timeout_ms / 1000),
                         .tv_nsec = (long)((timeout_ms % 1000) * 1000000L)};
  nanosleep(&req, NULL);
  return true;
}

void generator_capture_destroy(generator_capture_t* capture) { free(capture); }

void generator_capture_set_is_paused(generator_capture_t* capture,
                                     bool paused) {
  if (capture) {
    capture->is_paused = paused;
  }
}
