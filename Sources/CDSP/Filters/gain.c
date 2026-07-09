#include "gain.h"

struct gain_filter {
  char name[64];
  double linear_gain;
  bool muted;
};

#include <stdlib.h>
#include <string.h>

gain_filter_t* gain_filter_create(const char* name,
                                  const gain_parameters_t* params) {
  gain_filter_t* filter = (gain_filter_t*)malloc(sizeof(gain_filter_t));
  if (!filter) return NULL;
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "gain");
  }
  filter->muted = params ? params->mute : false;
  double gain_val = (params && params->has_gain) ? params->gain : 0.0;

  // Convert dB to linear gain if necessary, otherwise use linear gain directly.
  double computed_gain = (params && params->scale == GAIN_SCALE_LINEAR)
                             ? gain_val
                             : double_from_db(gain_val);

  // Apply phase inversion if configured.
  if (params && params->inverted) {
    computed_gain *= -1.0;
  }
  filter->linear_gain = computed_gain;
  return filter;
}

void gain_filter_process(gain_filter_t* filter, mutable_waveform_t waveform,
                         size_t count) {
  if (!filter || !waveform || count == 0) return;
  if (filter->muted) {
    // If muted, we clear the buffer to output silence.
    dsp_ops_clear(waveform, count);
  } else if (filter->linear_gain != 1.0) {
    // Apply linear scaling factor.
    dsp_ops_scalar_multiply(waveform, filter->linear_gain, count);
  }
}

double gain_filter_process_single(gain_filter_t* filter, double sample) {
  if (!filter || filter->muted) return 0.0;
  return sample * filter->linear_gain;
}

void gain_filter_free(gain_filter_t* filter) {
  if (filter) free(filter);
}
