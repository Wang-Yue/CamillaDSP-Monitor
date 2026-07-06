// Non-interleaved float buffers, one vector per channel.
#include "audio_chunk.h"

#include <stdlib.h>

/// Create a new silent audio chunk with freshly allocated storage.
audio_chunk_t* audio_chunk_create(size_t frames, size_t channels) {
  audio_chunk_t* chunk = (audio_chunk_t*)malloc(sizeof(audio_chunk_t));
  if (!chunk) return NULL;
  chunk->buffers = audio_buffers_create(channels, frames);
  if (!chunk->buffers) {
    free(chunk);
    return NULL;
  }
  chunk->valid_frames = frames;
  chunk->owns_buffers = true;
  return chunk;
}

/// Create an audio chunk that adopts the given `audio_buffers_t`. Zero-copy.
audio_chunk_t* audio_chunk_from_buffers(audio_buffers_t* buffers,
                                        size_t valid_frames) {
  if (!buffers) return NULL;
  audio_chunk_t* chunk = (audio_chunk_t*)malloc(sizeof(audio_chunk_t));
  if (!chunk) return NULL;
  chunk->buffers = buffers;
  chunk->valid_frames = valid_frames;
  chunk->owns_buffers = false;
  return chunk;
}

void audio_chunk_free(audio_chunk_t* chunk) {
  if (!chunk) return;
  if (chunk->owns_buffers && chunk->buffers) {
    audio_buffers_free(chunk->buffers);
  }
  free(chunk);
}

/// A preallocated round-robin pool of unique `audio_chunk_t` instances.
round_robin_chunk_pool_t* round_robin_chunk_pool_create(size_t capacity,
                                                        size_t frames,
                                                        size_t channels) {
  if (capacity == 0) return NULL;
  round_robin_chunk_pool_t* pool =
      (round_robin_chunk_pool_t*)calloc(1, sizeof(round_robin_chunk_pool_t));
  if (!pool) return NULL;
  pool->capacity = capacity;
  pool->current_index = 0;
  pool->pool = (audio_chunk_t**)calloc(capacity, sizeof(audio_chunk_t*));
  if (!pool->pool) {
    free(pool);
    return NULL;
  }
  for (size_t i = 0; i < capacity; i++) {
    pool->pool[i] = audio_chunk_create(frames, channels);
    if (!pool->pool[i]) {
      round_robin_chunk_pool_free(pool);
      return NULL;
    }
  }
  return pool;
}

/// Retrieves the next available unique chunk buffer from the pool.
audio_chunk_t* round_robin_chunk_pool_next(round_robin_chunk_pool_t* pool) {
  if (!pool || pool->capacity == 0) return NULL;
  audio_chunk_t* chunk = pool->pool[pool->current_index];
  pool->current_index = (pool->current_index + 1) % pool->capacity;
  return chunk;
}

void round_robin_chunk_pool_free(round_robin_chunk_pool_t* pool) {
  if (!pool) return;
  if (pool->pool) {
    for (size_t i = 0; i < pool->capacity; i++) {
      audio_chunk_free(pool->pool[i]);
    }
    free(pool->pool);
  }
  free(pool);
}
