// Heap-allocated, contiguous per-channel audio storage.
//
// Replaces nested array/pointer-to-pointer ("array of arrays") chunk storage.
#include "audio_buffers.h"
#include <stdlib.h>
#include <string.h>

/// Allocate a fresh buffer pool, zero-initialised.
audio_buffers_t* audio_buffers_create(size_t channels, size_t capacity) {
    if (channels == 0 || capacity == 0) return NULL;
    audio_buffers_t* buf = (audio_buffers_t*)malloc(sizeof(audio_buffers_t));
    if (!buf) return NULL;
    buf->channels = channels;
    buf->capacity = capacity;
    
    size_t total = channels * capacity;
    buf->storage = (double*)calloc(total, sizeof(double));
    buf->channel_buffers = (mutable_waveform_t*)malloc(channels * sizeof(mutable_waveform_t));
    
    for (size_t ch = 0; ch < channels; ch++) {
        buf->channel_buffers[ch] = buf->storage + (ch * capacity);
    }
    return buf;
}

/// Allocate a fresh buffer pool and copy existing waveform data into it.
audio_buffers_t* audio_buffers_copy_from(const double* const* waveforms, const size_t* channel_lengths, size_t channels) {
    if (channels == 0) return NULL;
    size_t max_cap = 0;
    for (size_t ch = 0; ch < channels; ch++) {
        if (channel_lengths[ch] > max_cap) max_cap = channel_lengths[ch];
    }
    if (max_cap == 0) max_cap = 1;
    
    audio_buffers_t* buf = audio_buffers_create(channels, max_cap);
    if (!buf) return NULL;
    
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
