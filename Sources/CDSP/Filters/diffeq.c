#include "diffeq.h"

struct diffeq_filter {
  char name[64];
  double* x;
  double* y;
  double* a;
  double* b;
  size_t a_count;
  size_t b_count;
  size_t idx_x;
  size_t idx_y;
};

#include <stdlib.h>
#include <string.h>

diffeq_filter_t* diffeq_filter_create(const char* name,
                                      const diff_eq_parameters_t* params) {
  diffeq_filter_t* filter = (diffeq_filter_t*)calloc(1, sizeof(diffeq_filter_t));
  if (!filter) return NULL;
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "diffeq");
  }

  size_t a_cnt =
      (params && params->a && params->a_count > 0) ? params->a_count : 1;
  size_t b_cnt =
      (params && params->b && params->b_count > 0) ? params->b_count : 1;

  filter->a_count = a_cnt;
  filter->b_count = b_cnt;

  filter->a = (double*)malloc(a_cnt * sizeof(double));
  filter->b = (double*)malloc(b_cnt * sizeof(double));
  filter->x = (double*)calloc(b_cnt, sizeof(double));
  filter->y = (double*)calloc(a_cnt, sizeof(double));

  if (!filter->a || !filter->b || !filter->x || !filter->y) {
    diffeq_filter_free(filter);
    return NULL;
  }

  if (params && params->a && params->a_count > 0) {
    memcpy(filter->a, params->a, a_cnt * sizeof(double));
  } else {
    filter->a[0] = 1.0;
  }
  if (params && params->b && params->b_count > 0) {
    memcpy(filter->b, params->b, b_cnt * sizeof(double));
  } else {
    filter->b[0] = 1.0;
  }

  // Normalize by a[0]
  if (filter->a[0] != 0.0 && filter->a[0] != 1.0) {
    double scale = 1.0 / filter->a[0];
    for (size_t i = 0; i < a_cnt; i++) filter->a[i] *= scale;
    for (size_t i = 0; i < b_cnt; i++) filter->b[i] *= scale;
  }

  filter->idx_x = 0;
  filter->idx_y = 0;
  return filter;
}

void diffeq_filter_process(diffeq_filter_t* filter, mutable_waveform_t waveform,
                           size_t count) {
  if (!filter || !waveform || count == 0) return;
  size_t nb = filter->b_count;
  size_t na = filter->a_count;
  double* x = filter->x;
  double* y = filter->y;
  const double* a = filter->a;
  const double* b = filter->b;
  size_t idx_x = filter->idx_x;
  size_t idx_y = filter->idx_y;

  // Process each sample through the difference equation:
  // y[n] = b[0]*x[n] + b[1]*x[n-1] + ... + b[N]*x[n-N] - a[1]*y[n-1] - ... -
  // a[M]*y[n-M] x and y are implemented as circular buffers to store historical
  // samples.
  for (size_t i = 0; i < count; i++) {
    // Advance circular buffer write indices
    idx_x++;
    if (idx_x >= nb) idx_x = 0;
    idx_y++;
    if (idx_y >= na) idx_y = 0;

    // Store current input sample
    x[idx_x] = waveform[i];

    double out = 0.0;
    // Compute feedforward part: sum(b[n] * x[n-i])
    int ptr_x = (int)idx_x;
    for (size_t n = 0; n < nb; n++) {
      out += b[n] * x[ptr_x];
      ptr_x--;
      if (ptr_x < 0) ptr_x = (int)nb - 1;
    }
    // Compute feedback part: sum(a[p] * y[p-j])
    int ptr_y = (int)idx_y - 1;
    if (ptr_y < 0) ptr_y = (int)na - 1;
    for (size_t p = 1; p < na; p++) {
      out -= a[p] * y[ptr_y];
      ptr_y--;
      if (ptr_y < 0) ptr_y = (int)na - 1;
    }

    // Store current output sample and update waveform
    y[idx_y] = out;
    waveform[i] = out;
  }
  filter->idx_x = idx_x;
  filter->idx_y = idx_y;
}

void diffeq_filter_free(diffeq_filter_t* filter) {
  if (!filter) return;
  if (filter->x) free(filter->x);
  if (filter->y) free(filter->y);
  if (filter->a) free(filter->a);
  if (filter->b) free(filter->b);
  free(filter);
}
