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
  diffeq_filter_t* filter = (diffeq_filter_t*)malloc(sizeof(diffeq_filter_t));
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
  // y[n] = b[0]*x[n] + b[1]*x[n-1] + ... + b[N]*x[n-N] - a[1]*y[n-1] - ... - a[M]*y[n-M]
  // x and y are implemented as circular buffers to store historical samples.
  for (size_t i = 0; i < count; i++) {
    // Advance circular buffer write indices
    idx_x = (idx_x + 1) % nb;
    idx_y = (idx_y + 1) % na;
    
    // Store current input sample
    x[idx_x] = waveform[i];

    double out = 0.0;
    // Compute feedforward part: sum(b[n] * x[n-i])
    for (size_t n = 0; n < nb; n++) {
      size_t n_idx = (idx_x + nb - n) % nb; // Retrieve x[n-i]
      out += b[n] * x[n_idx];
    }
    // Compute feedback part: sum(a[p] * y[p-j])
    for (size_t p = 1; p < na; p++) {
      size_t p_idx = (idx_y + na - p) % na; // Retrieve y[n-j]
      out -= a[p] * y[p_idx];
    }
    
    // Store current output sample and update waveform
    y[idx_y] = out;
    waveform[i] = out;
  }
  filter->idx_x = idx_x;
  filter->idx_y = idx_y;
}


void diffeq_filter_update_parameters(diffeq_filter_t* filter,
                                     const filter_config_t* config,
                                     int sample_rate) {
  (void)sample_rate;
  if (!filter || !config) return;
  if (config->type != FILTER_TYPE_DIFF_EQ) return;
  const diff_eq_parameters_t* params = &config->parameters.diff_eq;

  size_t a_cnt = (params->a && params->a_count > 0) ? params->a_count : 1;
  size_t b_cnt = (params->b && params->b_count > 0) ? params->b_count : 1;

  if (filter->a) free(filter->a);
  if (filter->b) free(filter->b);

  size_t old_b_count = filter->b_count;
  size_t old_a_count = filter->a_count;

  filter->a_count = a_cnt;
  filter->b_count = b_cnt;

  filter->a = (double*)malloc(a_cnt * sizeof(double));
  filter->b = (double*)malloc(b_cnt * sizeof(double));

  if (params->a && params->a_count > 0) {
    memcpy(filter->a, params->a, a_cnt * sizeof(double));
  } else {
    filter->a[0] = 1.0;
  }
  if (params->b && params->b_count > 0) {
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

  if (old_b_count != b_cnt) {
    if (filter->x) free(filter->x);
    filter->x = (double*)calloc(b_cnt, sizeof(double));
    filter->idx_x = 0;
  }
  if (old_a_count != a_cnt) {
    if (filter->y) free(filter->y);
    filter->y = (double*)calloc(a_cnt, sizeof(double));
    filter->idx_y = 0;
  }
}

void diffeq_filter_free(diffeq_filter_t* filter) {
  if (!filter) return;
  if (filter->x) free(filter->x);
  if (filter->y) free(filter->y);
  if (filter->a) free(filter->a);
  if (filter->b) free(filter->b);
  free(filter);
}
