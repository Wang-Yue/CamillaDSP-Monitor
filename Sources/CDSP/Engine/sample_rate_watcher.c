#include "sample_rate_watcher.h"

#include <stdlib.h>
#include <time.h>

#ifndef __APPLE__
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
static inline uint64_t clock_gettime_nsec_np(int clock_id) {
  struct timespec ts;
  clock_gettime(clock_id, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
#endif

struct sample_rate_watcher {
  double target_rate;
  double measure_interval;
  bool stop_on_rate_change;
  size_t captured_frames;
  uint64_t last_reset_ns;
  int deviation_count;
};

sample_rate_watcher_t* sample_rate_watcher_create(double target_rate,
                                                  double measure_interval,
                                                  bool stop_on_rate_change) {
  sample_rate_watcher_t* watcher =
      (sample_rate_watcher_t*)calloc(1, sizeof(sample_rate_watcher_t));
  if (!watcher) return NULL;
  watcher->target_rate = target_rate;
  watcher->measure_interval = measure_interval;
  watcher->stop_on_rate_change = stop_on_rate_change;
  watcher->captured_frames = 0;
  watcher->last_reset_ns = 0;
  watcher->deviation_count = 0;
  return watcher;
}

void sample_rate_watcher_free(sample_rate_watcher_t* watcher) { free(watcher); }

void sample_rate_watcher_reset(sample_rate_watcher_t* watcher) {
  if (!watcher) return;
  watcher->captured_frames = 0;
  watcher->last_reset_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  watcher->deviation_count = 0;
}

bool sample_rate_watcher_tick(sample_rate_watcher_t* watcher, size_t frames,
                              double* out_measured_rate) {
  if (!watcher) return false;
  if (watcher->last_reset_ns == 0) {
    watcher->last_reset_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  }
  watcher->captured_frames += frames;
  uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  double elapsed = (double)(now - watcher->last_reset_ns) / 1000000000.0;

  if (elapsed < watcher->measure_interval) {
    return false;
  }

  double measured_rate = (double)watcher->captured_frames / elapsed;
  watcher->captured_frames = 0;
  watcher->last_reset_ns = now;

  double min_val = watcher->target_rate / 1.04;
  double max_val = watcher->target_rate * 1.04;

  if (measured_rate < min_val || measured_rate > max_val) {
    watcher->deviation_count++;
  } else {
    watcher->deviation_count = 0;
  }

  if (watcher->deviation_count >= 3) {
    *out_measured_rate = measured_rate;
    return true;
  }
  return false;
}

bool sample_rate_watcher_get_stop_on_rate_change(
    const sample_rate_watcher_t* watcher) {
  return watcher ? watcher->stop_on_rate_change : false;
}
