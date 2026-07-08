#include <math.h>
#include <pthread.h>
#include <stdatomic.h>

#include "../../Sources/CDSP/Audio/lock_free_ring_buffer.h"
#include "test_support.h"

TEST(CapacityRoundsUpToPowerOfTwo) {
  spsc_audio_ring_buffer_t* r1 = spsc_audio_ring_buffer_create(1);
  ASSERT_EQ(2, spsc_audio_ring_buffer_get_capacity(r1));
  spsc_audio_ring_buffer_free(r1);

  spsc_audio_ring_buffer_t* r100 = spsc_audio_ring_buffer_create(100);
  ASSERT_EQ(128, spsc_audio_ring_buffer_get_capacity(r100));
  spsc_audio_ring_buffer_free(r100);

  spsc_audio_ring_buffer_t* r1024 = spsc_audio_ring_buffer_create(1024);
  ASSERT_EQ(1024, spsc_audio_ring_buffer_get_capacity(r1024));
  spsc_audio_ring_buffer_free(r1024);

  spsc_audio_ring_buffer_t* r1025 = spsc_audio_ring_buffer_create(1025);
  ASSERT_EQ(2048, spsc_audio_ring_buffer_get_capacity(r1025));
  spsc_audio_ring_buffer_free(r1025);
}

TEST(ReadLatestRequiresEnoughData) {
  spsc_audio_ring_buffer_t* ring = spsc_audio_ring_buffer_create(64);
  float dest[8];
  for (int i = 0; i < 8; i++) dest[i] = -1.0f;
  ASSERT_FALSE(spsc_audio_ring_buffer_read_latest(ring, dest, 8));

  double src[4] = {1.0, 2.0, 3.0, 4.0};
  spsc_audio_ring_buffer_append_converting_double_to_float(ring, src, 4);
  ASSERT_FALSE(spsc_audio_ring_buffer_read_latest(ring, dest, 8));
  for (int i = 0; i < 8; i++) {
    ASSERT_FLOAT_EQ(-1.0f, dest[i]);
  }
  spsc_audio_ring_buffer_free(ring);
}

TEST(RoundTripRespectsOrder) {
  spsc_audio_ring_buffer_t* ring = spsc_audio_ring_buffer_create(16);
  double src[] = {-1.0, -0.5, 0.0, 0.25, 0.5, 0.75, 1.0, 0.0};
  size_t count = sizeof(src) / sizeof(src[0]);
  spsc_audio_ring_buffer_append_converting_double_to_float(ring, src, count);
  float dest[8] = {0};
  ASSERT_TRUE(spsc_audio_ring_buffer_read_latest(ring, dest, count));
  for (size_t i = 0; i < count; i++) {
    ASSERT_NEAR((float)src[i], dest[i], 1e-7);
  }
  spsc_audio_ring_buffer_free(ring);
}

TEST(ReadLatestReturnsMostRecentAfterWrap) {
  spsc_audio_ring_buffer_t* ring = spsc_audio_ring_buffer_create(8);
  ASSERT_EQ(8, spsc_audio_ring_buffer_get_capacity(ring));
  double src[12];
  for (int i = 0; i < 12; i++) src[i] = (double)i;
  spsc_audio_ring_buffer_append_converting_double_to_float(ring, src, 12);
  float dest[8] = {0};
  ASSERT_TRUE(spsc_audio_ring_buffer_read_latest(ring, dest, 8));
  for (int i = 0; i < 8; i++) {
    ASSERT_FLOAT_EQ((float)(i + 4), dest[i]);
  }
  spsc_audio_ring_buffer_free(ring);
}

TEST(TotalSamplesWrittenIsMonotonic) {
  spsc_audio_ring_buffer_t* ring = spsc_audio_ring_buffer_create(64);
  ASSERT_EQ(0, spsc_audio_ring_buffer_get_total_samples_written(ring));
  double src[3] = {1.0, 2.0, 3.0};
  spsc_audio_ring_buffer_append_converting_double_to_float(ring, src, 3);
  spsc_audio_ring_buffer_append_converting_double_to_float(ring, src, 3);
  ASSERT_EQ(6, spsc_audio_ring_buffer_get_total_samples_written(ring));
  spsc_audio_ring_buffer_free(ring);
}

TEST(SpscRoundTripContiguous) {
  spsc_audio_ring_buffer_t* ring = spsc_audio_ring_buffer_create(16);
  float src[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
  spsc_audio_ring_buffer_write(ring, src, 6, 1);
  ASSERT_EQ(6, spsc_audio_ring_buffer_get_available_to_read(ring));
  float dest[6];
  for (int i = 0; i < 6; i++) dest[i] = -1.0f;
  size_t n = spsc_audio_ring_buffer_consume(ring, dest, 6);
  ASSERT_EQ(6, n);
  for (int i = 0; i < 6; i++) {
    ASSERT_FLOAT_EQ(src[i], dest[i]);
  }
  ASSERT_EQ(0, spsc_audio_ring_buffer_get_available_to_read(ring));
  spsc_audio_ring_buffer_free(ring);
}

TEST(SpscStridedWriteDeinterleaves) {
  float interleaved[] = {10.0f, 11.0f, 20.0f, 21.0f, 30.0f, 31.0f};
  spsc_audio_ring_buffer_t* ring = spsc_audio_ring_buffer_create(16);
  spsc_audio_ring_buffer_write(ring, interleaved + 1, 3, 2);
  float dest[3] = {0};
  spsc_audio_ring_buffer_consume(ring, dest, 3);
  ASSERT_FLOAT_EQ(11.0f, dest[0]);
  ASSERT_FLOAT_EQ(21.0f, dest[1]);
  ASSERT_FLOAT_EQ(31.0f, dest[2]);
  spsc_audio_ring_buffer_free(ring);
}

TEST(SpscConsumeReturnsLessThanRequestedOnUnderrun) {
  spsc_audio_ring_buffer_t* ring = spsc_audio_ring_buffer_create(16);
  float src[] = {1.0f, 2.0f, 3.0f};
  spsc_audio_ring_buffer_write(ring, src, 3, 1);
  float dest[8];
  for (int i = 0; i < 8; i++) dest[i] = -1.0f;
  size_t n = spsc_audio_ring_buffer_consume(ring, dest, 8);
  ASSERT_EQ(3, n);
  ASSERT_FLOAT_EQ(1.0f, dest[0]);
  ASSERT_FLOAT_EQ(2.0f, dest[1]);
  ASSERT_FLOAT_EQ(3.0f, dest[2]);
  ASSERT_FLOAT_EQ(-1.0f, dest[3]);
  ASSERT_EQ(0, spsc_audio_ring_buffer_get_available_to_read(ring));
  spsc_audio_ring_buffer_free(ring);
}

TEST(SpscWriteWrapsAroundCapacity) {
  spsc_audio_ring_buffer_t* ring = spsc_audio_ring_buffer_create(8);
  ASSERT_EQ(8, spsc_audio_ring_buffer_get_capacity(ring));
  float first_batch[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
  spsc_audio_ring_buffer_write(ring, first_batch, 6, 1);
  float dest[4] = {0};
  spsc_audio_ring_buffer_consume(ring, dest, 4);
  for (int i = 0; i < 4; i++) {
    ASSERT_FLOAT_EQ((float)(i + 1), dest[i]);
  }
  float second_batch[] = {7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f};
  spsc_audio_ring_buffer_write(ring, second_batch, 6, 1);
  float dest2[8] = {0};
  size_t n = spsc_audio_ring_buffer_consume(ring, dest2, 8);
  ASSERT_EQ(8, n);
  for (int i = 0; i < 8; i++) {
    ASSERT_FLOAT_EQ((float)(i + 5), dest2[i]);
  }
  spsc_audio_ring_buffer_free(ring);
}

TEST(SpscDrainResetsAvailable) {
  spsc_audio_ring_buffer_t* ring = spsc_audio_ring_buffer_create(8);
  float src[] = {1.0f, 2.0f, 3.0f, 4.0f};
  spsc_audio_ring_buffer_write(ring, src, 4, 1);
  ASSERT_EQ(4, spsc_audio_ring_buffer_get_available_to_read(ring));
  spsc_audio_ring_buffer_drain(ring);
  ASSERT_EQ(0, spsc_audio_ring_buffer_get_available_to_read(ring));
  spsc_audio_ring_buffer_free(ring);
}

typedef struct {
  spsc_audio_ring_buffer_t* ring;
  int total_to_write;
  int producer_chunk;
  _Atomic bool producer_done;
} spsc_concurrent_arg_t;

static void* spsc_producer_thread(void* arg) {
  spsc_concurrent_arg_t* a = (spsc_concurrent_arg_t*)arg;
  float counter = 0.0f;
  float* chunk = (float*)calloc(a->producer_chunk, sizeof(float));
  int written = 0;
  while (written < a->total_to_write) {
    for (int i = 0; i < a->producer_chunk; i++) {
      chunk[i] = counter++;
    }
    spsc_audio_ring_buffer_write(a->ring, chunk, a->producer_chunk, 1);
    written += a->producer_chunk;
  }
  free(chunk);
  atomic_store_explicit(&a->producer_done, true, memory_order_release);
  return NULL;
}

TEST(SpscConcurrentProducerConsumerNoDataLoss) {
  spsc_audio_ring_buffer_t* ring = spsc_audio_ring_buffer_create(65536);
  int producer_chunk = 256;
  int consumer_chunk = 64;
  int total_to_write = 50176;

  spsc_concurrent_arg_t arg = {ring, total_to_write, producer_chunk, false};
  pthread_t th;
  pthread_create(&th, NULL, spsc_producer_thread, &arg);

  float* dest = (float*)calloc(consumer_chunk, sizeof(float));
  float last_seen = -1.0f;
  int consumed = 0;

  while (consumed < total_to_write) {
    size_t n = spsc_audio_ring_buffer_consume(ring, dest, consumer_chunk);
    if (n > 0) {
      ASSERT_TRUE(dest[0] > last_seen);
      for (size_t i = 1; i < n; i++) {
        ASSERT_NEAR(dest[i - 1] + 1.0f, dest[i], 1e-3);
      }
      last_seen = dest[n - 1];
      consumed += (int)n;
    }
    if (atomic_load_explicit(&arg.producer_done, memory_order_acquire)) {
      while (true) {
        size_t m = spsc_audio_ring_buffer_consume(ring, dest, consumer_chunk);
        if (m == 0) break;
        consumed += (int)m;
      }
      break;
    }
  }
  pthread_join(th, NULL);
  free(dest);
  spsc_audio_ring_buffer_free(ring);
  ASSERT_EQ(total_to_write, consumed);
}

TEST(SpscQueueRoundTripFifo) {
  spsc_queue_t* queue = spsc_queue_create(8);
  ASSERT_EQ(8, spsc_queue_get_capacity(queue));
  ASSERT_EQ(0, spsc_queue_get_count(queue));
  ASSERT_TRUE(spsc_queue_dequeue(queue) == NULL);
  for (intptr_t i = 1; i <= 5; i++) {
    ASSERT_TRUE(spsc_queue_enqueue(queue, (void*)i));
  }
  ASSERT_EQ(5, spsc_queue_get_count(queue));
  for (intptr_t i = 1; i <= 5; i++) {
    ASSERT_EQ((void*)i, spsc_queue_dequeue(queue));
  }
  ASSERT_EQ(0, spsc_queue_get_count(queue));
  ASSERT_TRUE(spsc_queue_dequeue(queue) == NULL);
  spsc_queue_free(queue);
}

TEST(SpscQueueEnqueueReturnsFalseWhenFull) {
  spsc_queue_t* queue = spsc_queue_create(4);
  for (intptr_t i = 1; i <= 4; i++) {
    ASSERT_TRUE(spsc_queue_enqueue(queue, (void*)i));
  }
  ASSERT_FALSE(spsc_queue_enqueue(queue, (void*)99));
  ASSERT_EQ((void*)1, spsc_queue_dequeue(queue));
  ASSERT_TRUE(spsc_queue_enqueue(queue, (void*)99));
  spsc_queue_free(queue);
}

TEST(SpscQueueWrapsAroundIndices) {
  spsc_queue_t* queue = spsc_queue_create(4);
  for (intptr_t i = 1; i <= 3; i++) {
    ASSERT_TRUE(spsc_queue_enqueue(queue, (void*)i));
  }
  for (intptr_t i = 1; i <= 3; i++) {
    ASSERT_EQ((void*)i, spsc_queue_dequeue(queue));
  }
  for (intptr_t i = 100; i < 104; i++) {
    ASSERT_TRUE(spsc_queue_enqueue(queue, (void*)i));
  }
  for (intptr_t i = 100; i < 104; i++) {
    ASSERT_EQ((void*)i, spsc_queue_dequeue(queue));
  }
  spsc_queue_free(queue);
}

typedef struct {
  int value;
} non_sendable_item_t;

TEST(SpscQueueTransferNonSendable) {
  spsc_queue_t* queue = spsc_queue_create(4);
  non_sendable_item_t item = {42};
  ASSERT_TRUE(spsc_queue_enqueue(queue, &item));
  non_sendable_item_t* popped = (non_sendable_item_t*)spsc_queue_dequeue(queue);
  ASSERT_TRUE(popped != NULL);
  ASSERT_EQ(42, popped->value);
  spsc_queue_free(queue);
}

typedef struct {
  spsc_queue_t* queue;
  int total_to_write;
} queue_concurrent_arg_t;

static void* queue_producer_thread(void* arg) {
  queue_concurrent_arg_t* a = (queue_concurrent_arg_t*)arg;
  int i = 0;
  while (i < a->total_to_write) {
    if (spsc_queue_enqueue(a->queue, (void*)(intptr_t)(i + 1))) {
      i++;
    }
  }
  return NULL;
}

TEST(SpscQueueConcurrentNoDataLoss) {
  spsc_queue_t* queue = spsc_queue_create(64);
  int total_to_write = 200000;
  queue_concurrent_arg_t arg = {queue, total_to_write};
  pthread_t th;
  pthread_create(&th, NULL, queue_producer_thread, &arg);

  int last_seen = 0;
  int consumed = 0;
  while (consumed < total_to_write) {
    void* val = spsc_queue_dequeue(queue);
    if (val) {
      int v = (int)(intptr_t)val;
      ASSERT_EQ(last_seen + 1, v);
      last_seen = v;
      consumed++;
    }
  }
  pthread_join(th, NULL);
  spsc_queue_free(queue);
  ASSERT_EQ(total_to_write, consumed);
}

TEST(AtomicDoubleRoundTrip) {
  atomic_double_t* value = atomic_double_create(1.5);
  ASSERT_DOUBLE_EQ(1.5, atomic_double_get(value));
  atomic_double_set(value, 2.71828);
  ASSERT_DOUBLE_EQ(2.71828, atomic_double_get(value));
  atomic_double_set(value, -0.0);
  double d = atomic_double_get(value);
  uint64_t u1, u2;
  memcpy(&u1, &d, sizeof(uint64_t));
  double minus_zero = -0.0;
  memcpy(&u2, &minus_zero, sizeof(uint64_t));
  ASSERT_EQ(u2, u1);
  atomic_double_set(value, INFINITY);
  ASSERT_DOUBLE_EQ(INFINITY, atomic_double_get(value));
  atomic_double_free(value);
}

typedef struct {
  spsc_audio_ring_buffer_t* ring;
  int total_to_write;
  int chunk_size;
  _Atomic bool producer_done;
} spmc_concurrent_arg_t;

static void* spmc_producer_thread(void* arg) {
  spmc_concurrent_arg_t* a = (spmc_concurrent_arg_t*)arg;
  double counter = 0.0;
  double* chunk = (double*)calloc(a->chunk_size, sizeof(double));
  int written = 0;
  while (written < a->total_to_write) {
    for (int i = 0; i < a->chunk_size; i++) {
      chunk[i] = counter++;
    }
    spsc_audio_ring_buffer_append_converting_double_to_float(a->ring, chunk,
                                                             a->chunk_size);
    written += a->chunk_size;
  }
  free(chunk);
  atomic_store_explicit(&a->producer_done, true, memory_order_release);
  return NULL;
}

TEST(ConcurrentProducerConsumerSeesMonotonicSequence) {
  spsc_audio_ring_buffer_t* ring = spsc_audio_ring_buffer_create(4096);
  int total_to_write = 200000;
  int chunk_size = 256;
  int read_size = 64;
  spmc_concurrent_arg_t arg = {ring, total_to_write, chunk_size, false};
  pthread_t th;
  pthread_create(&th, NULL, spmc_producer_thread, &arg);

  int snapshots_taken = 0;
  float* dest = (float*)calloc(read_size, sizeof(float));
  while (!atomic_load_explicit(&arg.producer_done, memory_order_acquire)) {
    bool ok = spsc_audio_ring_buffer_read_latest(ring, dest, read_size);
    if (ok) {
      for (int i = 1; i < read_size; i++) {
        ASSERT_NEAR(dest[i - 1] + 1.0f, dest[i], 1e-3);
      }
      snapshots_taken++;
    }
  }
  pthread_join(th, NULL);
  free(dest);
  ASSERT_TRUE(snapshots_taken > 0);
  ASSERT_TRUE(spsc_audio_ring_buffer_get_total_samples_written(ring) >=
              (uint64_t)total_to_write);
  spsc_audio_ring_buffer_free(ring);
}

TEST_MAIN()
