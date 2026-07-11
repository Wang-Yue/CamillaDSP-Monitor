// Heap-allocated, contiguous per-channel audio storage.
//
// Replaces nested array/pointer-to-pointer ("array of arrays") chunk storage.
#include "audio_buffers.h"

#include <stdlib.h>
#include <string.h>

struct audio_buffers {
  size_t channels;
  size_t capacity;
  double* storage;
  mutable_waveform_t* channel_buffers;
};

size_t audio_buffers_get_channels(const audio_buffers_t* buffers) {
  return buffers ? buffers->channels : 0;
}

size_t audio_buffers_get_capacity(const audio_buffers_t* buffers) {
  return buffers ? buffers->capacity : 0;
}

mutable_waveform_t audio_buffers_get_channel(const audio_buffers_t* buffers,
                                             size_t ch) {
  if (!buffers || ch >= buffers->channels) return NULL;
  return buffers->channel_buffers[ch];
}

/// Allocate a fresh buffer pool, zero-initialised.
audio_buffers_t* audio_buffers_create(size_t channels, size_t capacity) {
  if (channels == 0 || capacity == 0) return NULL;

  // Check for overflow before multiplication
  if (capacity > SIZE_MAX / channels) return NULL;

  audio_buffers_t* buf = (audio_buffers_t*)malloc(sizeof(audio_buffers_t));
  if (!buf) return NULL;

  buf->channels = channels;
  buf->capacity = capacity;
  buf->storage = NULL;
  buf->channel_buffers = NULL;

  // Allocate a single contiguous block of memory for all channels to improve
  // cache locality and reduce memory fragmentation.
  size_t total = channels * capacity;
  buf->storage = (double*)calloc(total, sizeof(double));
  // Allocate the array of pointers that will point to the start of each
  // channel's buffer.
  buf->channel_buffers =
      (mutable_waveform_t*)malloc(channels * sizeof(mutable_waveform_t));

  if (!buf->storage || !buf->channel_buffers) {
    audio_buffers_free(buf);
    return NULL;
  }

  // Set up pointers to point into the contiguous block.
  for (size_t ch = 0; ch < channels; ch++) {
    buf->channel_buffers[ch] = buf->storage + (ch * capacity);
  }
  return buf;
}

/// Allocate a fresh buffer pool and copy existing waveform data into it.
audio_buffers_t* audio_buffers_copy_from(const double* const* waveforms,
                                         const size_t* channel_lengths,
                                         size_t channels) {
  if (channels == 0) return NULL;

  // Find the maximum length among all channels to determine the capacity of the
  // new buffer.
  size_t max_cap = 0;
  for (size_t ch = 0; ch < channels; ch++) {
    if (channel_lengths[ch] > max_cap) max_cap = channel_lengths[ch];
  }
  if (max_cap == 0) max_cap = 1;

  audio_buffers_t* buf = audio_buffers_create(channels, max_cap);
  if (!buf) return NULL;

  // Copy data from the potentially non-contiguous source waveforms into our
  // contiguous storage.
  for (size_t ch = 0; ch < channels; ch++) {
    size_t len = channel_lengths[ch];
    if (len > 0 && waveforms[ch]) {
      memcpy(buf->channel_buffers[ch], waveforms[ch], len * sizeof(double));
    }
  }
  return buf;
}

void audio_buffers_free(audio_buffers_t* buffers) {
  if (!buffers) return;
  if (buffers->storage) free(buffers->storage);
  if (buffers->channel_buffers) free(buffers->channel_buffers);
  free(buffers);
}
