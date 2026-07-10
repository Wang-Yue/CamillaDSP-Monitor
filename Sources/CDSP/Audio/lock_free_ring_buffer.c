// Single-producer / single-consumer lock-free primitives used by the audio
// thread.
#include "Audio/lock_free_ring_buffer.h"

#include <stdlib.h>

struct spsc_audio_ring_buffer {
  size_t capacity;
  size_t mask;
  float* storage;
  _Atomic uint64_t write_index;
  _Atomic uint64_t read_index;
};

struct spsc_queue {
  size_t capacity;
  size_t mask;
  void** storage;
  _Atomic uint64_t write_index;
  _Atomic uint64_t read_index;
};

uint64_t spsc_audio_ring_buffer_get_total_samples_written(
    const spsc_audio_ring_buffer_t* ring) {
  return atomic_load_explicit(&ring->write_index, memory_order_relaxed);
}

size_t spsc_audio_ring_buffer_get_available_to_read(
    const spsc_audio_ring_buffer_t* ring) {
  // Acquire barrier on write_index ensures that any data written by the
  // producer prior to updating write_index is visible to this thread.
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_acquire);
  // Relaxed load is sufficient for read_index as it is only modified by the
  // consumer (which is typically the calling thread of this function).
  uint64_t r = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
  // Unsigned subtraction correctly handles overflow wrap-around.
  return (size_t)(w - r);
}

size_t spsc_audio_ring_buffer_get_available_to_write(
    const spsc_audio_ring_buffer_t* ring) {
  // Relaxed load is sufficient for write_index as it is only modified by the
  // producer (which is typically the calling thread).
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
  // Acquire barrier on read_index ensures we see the consumer's latest read
  // index before we decide how much space we can write to, avoiding overwriting
  // unread data.
  uint64_t r = atomic_load_explicit(&ring->read_index, memory_order_acquire);
  size_t occupied = (size_t)(w - r);
  if (occupied >= ring->capacity) {
    return 0;
  }
  return ring->capacity - occupied;
}

size_t spsc_audio_ring_buffer_get_capacity(
    const spsc_audio_ring_buffer_t* ring) {
  return ring ? ring->capacity : 0;
}

size_t spsc_queue_get_count(const spsc_queue_t* queue) {
  uint64_t w = atomic_load_explicit(&queue->write_index, memory_order_acquire);
  uint64_t r = atomic_load_explicit(&queue->read_index, memory_order_relaxed);
  return (size_t)(w - r);
}

size_t spsc_queue_get_capacity(const spsc_queue_t* queue) {
  return queue ? queue->capacity : 0;
}
#include <string.h>

#ifdef ENABLE_ACCELERATE
#include <Accelerate/Accelerate.h>
#endif

// MARK: - SPSCAudioRingBuffer Implementation

spsc_audio_ring_buffer_t* spsc_audio_ring_buffer_create(
    size_t minimum_capacity) {
  size_t cap = spsc_audio_ring_buffer_round_up_to_power_of_two(
      minimum_capacity < 2 ? 2 : minimum_capacity);
  spsc_audio_ring_buffer_t* ring =
      (spsc_audio_ring_buffer_t*)malloc(sizeof(spsc_audio_ring_buffer_t));
  if (!ring) return NULL;
  ring->capacity = cap;
  ring->mask = cap - 1;
  ring->storage = (float*)calloc(cap, sizeof(float));
  if (!ring->storage) {
    free(ring);
    return NULL;
  }
  atomic_init(&ring->write_index, 0);
  atomic_init(&ring->read_index, 0);
  return ring;
}

void spsc_audio_ring_buffer_free(spsc_audio_ring_buffer_t* ring) {
  if (!ring) return;
  free(ring->storage);
  free(ring);
}

void spsc_audio_ring_buffer_write(spsc_audio_ring_buffer_t* ring,
                                  const float* source, size_t count,
                                  size_t stride) {
  if (count == 0 || !ring || !source) return;
  const float* src = source;
  size_t cnt = count;
  // If count exceeds capacity, only write the most recent 'capacity' elements.
  if (cnt > ring->capacity) {
    size_t skip = cnt - ring->capacity;
    src += skip * stride;
    cnt = ring->capacity;
  }
  // Relaxed load: we are the only writer.
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
  size_t write_offset = (size_t)(w & ring->mask);

  // Calculate how many samples we can write before wrapping around the end of
  // the ring.
  size_t first_chunk = ring->capacity - write_offset;
  if (first_chunk > cnt) first_chunk = cnt;

  if (stride == 1) {
    memcpy(ring->storage + write_offset, src, first_chunk * sizeof(float));
    if (first_chunk < cnt) {
      memcpy(ring->storage, src + first_chunk,
             (cnt - first_chunk) * sizeof(float));
    }
  } else {
#ifdef ENABLE_ACCELERATE
    // Strided copy: extract every `stride`-th element of `source`
    // into the contiguous ring slot. `vDSP_vsadd` with a zero
    // scalar is used as a stride-aware copy because vDSP lacks a
    // dedicated strided memcpy.
    float zero = 0.0f;
    vDSP_vsadd(src, stride, &zero, ring->storage + write_offset, 1,
               first_chunk);
    if (first_chunk < cnt) {
      vDSP_vsadd(src + (stride * first_chunk), stride, &zero, ring->storage, 1,
                 cnt - first_chunk);
    }
#else
    for (size_t i = 0; i < first_chunk; i++) {
      ring->storage[write_offset + i] = src[i * stride];
    }
    if (first_chunk < cnt) {
      for (size_t i = 0; i < cnt - first_chunk; i++) {
        ring->storage[i] = src[(first_chunk + i) * stride];
      }
    }
#endif
  }
  // Release store: makes all previous memory writes (the data copy) visible to
  // the reader before they see the updated write_index.
  atomic_store_explicit(&ring->write_index, w + cnt, memory_order_release);
}

void spsc_audio_ring_buffer_append_converting_double_to_float(
    spsc_audio_ring_buffer_t* ring, const double* source, size_t count) {
  if (count == 0 || !ring || !source) return;
  const double* src = source;
  size_t cnt = count;
  if (cnt > ring->capacity) {
    size_t skip = cnt - ring->capacity;
    src += skip;
    cnt = ring->capacity;
  }
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
  size_t write_offset = (size_t)(w & ring->mask);
  size_t first_chunk = ring->capacity - write_offset;
  if (first_chunk > cnt) first_chunk = cnt;

#ifdef ENABLE_ACCELERATE
  // vDSP_vdpsp: convert and store Double->Float, no allocation.
  vDSP_vdpsp(src, 1, ring->storage + write_offset, 1, first_chunk);
  if (first_chunk < cnt) {
    vDSP_vdpsp(src + first_chunk, 1, ring->storage, 1, cnt - first_chunk);
  }
#else
  for (size_t i = 0; i < first_chunk; i++) {
    ring->storage[write_offset + i] = (float)src[i];
  }
  if (first_chunk < cnt) {
    for (size_t i = 0; i < cnt - first_chunk; i++) {
      ring->storage[i] = (float)src[first_chunk + i];
    }
  }
#endif
  atomic_store_explicit(&ring->write_index, w + cnt, memory_order_release);
}

void spsc_audio_ring_buffer_write_silence(spsc_audio_ring_buffer_t* ring,
                                          size_t count) {
  if (count == 0 || !ring) return;
  size_t cnt = count;
  if (cnt > ring->capacity) {
    cnt = ring->capacity;
  }
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
  size_t write_offset = (size_t)(w & ring->mask);
  size_t first_chunk = ring->capacity - write_offset;
  if (first_chunk > cnt) first_chunk = cnt;

  memset(ring->storage + write_offset, 0, first_chunk * sizeof(float));
  if (first_chunk < cnt) {
    memset(ring->storage, 0, (cnt - first_chunk) * sizeof(float));
  }
  atomic_store_explicit(&ring->write_index, w + cnt, memory_order_release);
}

size_t spsc_audio_ring_buffer_consume(spsc_audio_ring_buffer_t* ring,
                                      float* dest, size_t count) {
  if (count == 0 || !ring || !dest) return 0;
  // Relaxed load: we are the only reader.
  uint64_t r = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
  // Acquire barrier: ensure we see the writes to the buffer elements before we
  // read them.
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_acquire);
  size_t avail = (size_t)(w - r);

  if (avail > ring->capacity) {
    // Producer has overwritten unread data. This can happen if the producer
    // writes faster than we consume and the buffer wraps around. Advance read
    // pointer to the oldest valid data (write_index - capacity).
    r = w - (uint64_t)ring->capacity;
    atomic_store_explicit(&ring->read_index, r, memory_order_release);
    avail = ring->capacity;
  }

  size_t n = avail < count ? avail : count;
  if (n == 0) return 0;

  // Calculate read offset and handle wrap-around.
  size_t read_offset = (size_t)(r & ring->mask);
  size_t first_chunk = ring->capacity - read_offset;
  if (first_chunk > n) first_chunk = n;

  memcpy(dest, ring->storage + read_offset, first_chunk * sizeof(float));
  if (first_chunk < n) {
    memcpy(dest + first_chunk, ring->storage,
           (n - first_chunk) * sizeof(float));
  }
  // Release store: let the producer know we've freed up slot space up to r + n.
  atomic_store_explicit(&ring->read_index, r + n, memory_order_release);
  return n;
}

size_t spsc_audio_ring_buffer_consume_stride(spsc_audio_ring_buffer_t* ring,
                                             float* dest, size_t count,
                                             size_t stride) {
  if (count == 0 || !ring || !dest) return 0;
  // Relaxed load: we are the only reader.
  uint64_t r = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
  // Acquire barrier: ensure we see the writes to the buffer elements before we
  // read them.
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_acquire);
  size_t avail = (size_t)(w - r);

  if (avail > ring->capacity) {
    // Producer has overwritten unread data. This can happen if the producer
    // writes faster than we consume and the buffer wraps around. Advance read
    // pointer to the oldest valid data (write_index - capacity).
    r = w - (uint64_t)ring->capacity;
    atomic_store_explicit(&ring->read_index, r, memory_order_release);
    avail = ring->capacity;
  }

  size_t n = avail < count ? avail : count;
  if (n == 0) return 0;

  // Calculate read offset and handle wrap-around.
  size_t read_offset = (size_t)(r & ring->mask);
  size_t first_chunk = ring->capacity - read_offset;
  if (first_chunk > n) first_chunk = n;

  if (stride == 1) {
    memcpy(dest, ring->storage + read_offset, first_chunk * sizeof(float));
    if (first_chunk < n) {
      memcpy(dest + first_chunk, ring->storage,
             (n - first_chunk) * sizeof(float));
    }
  } else {
#ifdef ENABLE_ACCELERATE
    float zero = 0.0f;
    vDSP_vsadd(ring->storage + read_offset, 1, &zero, dest, stride,
               first_chunk);
    if (first_chunk < n) {
      vDSP_vsadd(ring->storage, 1, &zero, dest + (stride * first_chunk), stride,
                 n - first_chunk);
    }
#else
    for (size_t i = 0; i < first_chunk; i++) {
      dest[i * stride] = ring->storage[read_offset + i];
    }
    if (first_chunk < n) {
      for (size_t i = 0; i < n - first_chunk; i++) {
        dest[(first_chunk + i) * stride] = ring->storage[i];
      }
    }
#endif
  }
  // Release store: let the producer know we've freed up slot space up to r + n.
  atomic_store_explicit(&ring->read_index, r + n, memory_order_release);
  return n;
}

void spsc_audio_ring_buffer_drain(spsc_audio_ring_buffer_t* ring) {
  if (!ring) return;
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_acquire);
  atomic_store_explicit(&ring->read_index, w, memory_order_release);
}

bool spsc_audio_ring_buffer_read_latest_at(const spsc_audio_ring_buffer_t* ring,
                                           float* dest, size_t count,
                                           uint64_t written) {
  if (!ring || !dest || count == 0 || count > ring->capacity) return false;
  if (written < (uint64_t)count) return false;

  uint64_t written_before =
      atomic_load_explicit(&ring->write_index, memory_order_acquire);
  if (written_before - written > (uint64_t)(ring->capacity - count)) {
    return false;
  }

  size_t end_idx = (size_t)(written & ring->mask);
  size_t start_idx = (end_idx + ring->capacity - count) & ring->mask;
  size_t first_chunk = ring->capacity - start_idx;
  if (first_chunk > count) first_chunk = count;

  memcpy(dest, ring->storage + start_idx, first_chunk * sizeof(float));
  if (first_chunk < count) {
    memcpy(dest + first_chunk, ring->storage,
           (count - first_chunk) * sizeof(float));
  }

  uint64_t written_after =
      atomic_load_explicit(&ring->write_index, memory_order_acquire);
  if (written_after - written > (uint64_t)(ring->capacity - count)) {
    return false;
  }
  return true;
}

bool spsc_audio_ring_buffer_read_latest(const spsc_audio_ring_buffer_t* ring,
                                        float* dest, size_t count) {
  if (!ring || !dest || count == 0 || count > ring->capacity) return false;

  int retries = 5;
  while (retries > 0) {
    uint64_t written =
        atomic_load_explicit(&ring->write_index, memory_order_acquire);
    if (written >= (uint64_t)count) {
      if (spsc_audio_ring_buffer_read_latest_at(ring, dest, count, written)) {
        return true;
      }
    } else {
      return false;
    }
    retries--;
  }
  return false;
}

// MARK: - SPSCQueue Implementation

spsc_queue_t* spsc_queue_create(size_t minimum_capacity) {
  size_t cap = spsc_audio_ring_buffer_round_up_to_power_of_two(
      minimum_capacity < 2 ? 2 : minimum_capacity);
  spsc_queue_t* queue = (spsc_queue_t*)malloc(sizeof(spsc_queue_t));
  if (!queue) return NULL;
  queue->capacity = cap;
  queue->mask = cap - 1;
  queue->storage = (void**)calloc(cap, sizeof(void*));
  if (!queue->storage) {
    free(queue);
    return NULL;
  }
  atomic_init(&queue->write_index, 0);
  atomic_init(&queue->read_index, 0);
  return queue;
}

void spsc_queue_free(spsc_queue_t* queue) {
  if (!queue) return;
  // Clearing each slot to NULL; deinitialize then deallocate the raw storage.
  free(queue->storage);
  free(queue);
}

bool spsc_queue_enqueue(spsc_queue_t* queue, void* value) {
  if (!queue) return false;
  uint64_t w = atomic_load_explicit(&queue->write_index, memory_order_relaxed);
  uint64_t r = atomic_load_explicit(&queue->read_index, memory_order_acquire);
  if (w - r >= (uint64_t)queue->capacity) return false;
  queue->storage[(size_t)(w & queue->mask)] = value;
  atomic_store_explicit(&queue->write_index, w + 1, memory_order_release);
  return true;
}

void* spsc_queue_dequeue(spsc_queue_t* queue) {
  if (!queue) return NULL;
  uint64_t r = atomic_load_explicit(&queue->read_index, memory_order_relaxed);
  uint64_t w = atomic_load_explicit(&queue->write_index, memory_order_acquire);
  if (r == w) return NULL;
  size_t slot = (size_t)(r & queue->mask);
  void* value = queue->storage[slot];
  queue->storage[slot] = NULL;
  atomic_store_explicit(&queue->read_index, r + 1, memory_order_release);
  return value;
}

void spsc_queue_drain(spsc_queue_t* queue) {
  if (!queue) return;
  while (spsc_queue_dequeue(queue) != NULL) {
  }
}

// MARK: - AtomicDouble Implementation

atomic_double_t* atomic_double_create(double value) {
  atomic_double_t* a = (atomic_double_t*)malloc(sizeof(atomic_double_t));
  if (!a) return NULL;
  atomic_double_init(a, value);
  return a;
}

void atomic_double_free(atomic_double_t* a) {
  if (!a) return;
  free(a);
}
