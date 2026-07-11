#include "limiter.h"

struct limiter_filter {
  char name[64];
  double clip_limit;
  bool soft_clip;
};

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ENABLE_ACCELERATE
#include <Accelerate/Accelerate.h>
#endif

limiter_filter_t* limiter_filter_create(const char* name,
                                        const limiter_parameters_t* params) {
  limiter_filter_t* filter =
      (limiter_filter_t*)calloc(1, sizeof(limiter_filter_t));
  if (!filter) return NULL;
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "limiter");
  }
  double limit_db = params ? params->clip_limit : 0.0;
  double limit = double_from_db(limit_db);
  if (limit <= 0.0 || !isfinite(limit)) {
    limiter_filter_free(filter);
    return NULL;
  }
  filter->clip_limit = limit;
  filter->soft_clip = params ? params->soft_clip : false;
  return filter;
}

void limiter_filter_process(limiter_filter_t* filter,
                            mutable_waveform_t waveform, size_t count) {
  if (!filter || !waveform || count == 0) return;
  if (filter->soft_clip) {
    double inv_limit = 1.0 / filter->clip_limit;
    for (size_t i = 0; i < count; i++) {
      // Scale waveform to range relative to clip limit.
      double scaled = waveform[i] * inv_limit;

      // Clamp scaled value to [-1.5, 1.5]. The soft clipping function
      // f(x) = x - x^3 / 6.75 is designed for this range, mapping it
      // smoothly to [-1.0, 1.0] with a flat slope at the boundaries.
      if (scaled < -1.5)
        scaled = -1.5;
      else if (scaled > 1.5)
        scaled = 1.5;

      // Apply the cubic soft-clipping polynomial: f(x) = x - x^3 / 6.75
      // and scale it back to the original clip limit.
      waveform[i] =
          (scaled - (scaled * scaled * scaled) / 6.75) * filter->clip_limit;
    }
  } else {
    double low_limit = -filter->clip_limit;
    double high_limit = filter->clip_limit;
#ifdef ENABLE_ACCELERATE
    vDSP_vclipD(waveform, 1, &low_limit, &high_limit, waveform, 1, count);
#else
    for (size_t i = 0; i < count; i++) {
      if (waveform[i] < low_limit)
        waveform[i] = low_limit;
      else if (waveform[i] > high_limit)
        waveform[i] = high_limit;
    }
#endif
  }
}

void limiter_filter_free(limiter_filter_t* filter) {
  if (filter) free(filter);
}
