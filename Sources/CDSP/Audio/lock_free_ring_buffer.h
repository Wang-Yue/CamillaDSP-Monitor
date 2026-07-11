/**
 * @file lock_free_ring_buffer.h
 * @brief Single-producer / single-consumer lock-free primitives used by the
 * audio thread.
 *
 * Contains:
 * - @ref spsc_audio_ring_buffer_t: Ring buffer of `float` samples.
 * - @ref spsc_queue_t: Generic SPSC FIFO queue for pointers.
 * - @ref atomic_double_t: Wait-free atomic `double`.
 *
 * Real-time discipline:
 * All hot-path methods are wait-free, allocation-free, and free of
 * runtime calls or syscalls that could block. The producer always succeeds
 * — if the consumer is so far behind that the buffer is full, the
 * oldest unread data is silently overwritten (matching the original
 * lock-based design's drop-on-overflow behaviour).
 */

#ifndef CLIB_AUDIO_LOCK_FREE_RING_BUFFER_H
#define CLIB_AUDIO_LOCK_FREE_RING_BUFFER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// MARK: - SPSCAudioRingBuffer

/**
 * @struct spsc_audio_ring_buffer
 * @brief Lock-free SPSC ring buffer of `float` samples.
 *
 * Power-of-two capacity so wrap-around is a single bitmask. Producer publishes
 * new samples with a `release` store on `write_index`; consumers observe with
 * an `acquire` load, establishing happens-before without locks.
 *
 * Two consumer styles, both supported on the same instance — but don't mix them
 * on a single ring:
 *
 * - **Consume:** call @ref spsc_audio_ring_buffer_consume to drain samples.
 *   Each sample is delivered to exactly one consumer call.
 *   Used by audio capture and playback paths.
 * - **Snapshot:** call @ref spsc_audio_ring_buffer_read_latest to copy the
 *   most-recent `count` samples without advancing any cursor.
 *   The same samples can be re-read across calls. Used by
 *   @ref spectrum_analyzer_t to feed FFTs at different lengths.
 */
typedef struct spsc_audio_ring_buffer spsc_audio_ring_buffer_t;

/**
 * @brief Create a new SPSC audio ring buffer.
 *
 * @param minimum_capacity The minimum requested capacity. The actual capacity
 *                         will be rounded up to the next power of two.
 * @return Pointer to the allocated buffer, or NULL on failure.
 */
spsc_audio_ring_buffer_t* spsc_audio_ring_buffer_create(
    size_t minimum_capacity);

/**
 * @brief Free the SPSC audio ring buffer.
 *
 * @param ring Pointer to the ring buffer to free.
 */
void spsc_audio_ring_buffer_free(spsc_audio_ring_buffer_t* ring);

/**
 * @brief Get total samples written since allocation.
 *
 * @param ring Pointer to the ring buffer.
 * @return Total number of samples written.
 */
uint64_t spsc_audio_ring_buffer_get_total_samples_written(
    const spsc_audio_ring_buffer_t* ring);

/**
 * @brief Number of samples currently waiting to be consumed.
 *
 * For consume-style use. Always non-negative.
 *
 * @param ring Pointer to the ring buffer.
 * @return Number of available samples to read.
 */
size_t spsc_audio_ring_buffer_get_available_to_read(
    const spsc_audio_ring_buffer_t* ring);

/**
 * @brief Number of samples that can be written without overwriting unread data.
 *
 * @param ring Pointer to the ring buffer.
 * @return Number of available slots to write.
 */
size_t spsc_audio_ring_buffer_get_available_to_write(
    const spsc_audio_ring_buffer_t* ring);

/**
 * @brief Get the capacity of the ring buffer.
 *
 * @param ring Pointer to the ring buffer.
 * @return The capacity.
 */
size_t spsc_audio_ring_buffer_get_capacity(
    const spsc_audio_ring_buffer_t* ring);

// MARK: Producer

/**
 * @brief Write samples into the ring buffer.
 *
 * **Producer-only.** Write `count` `float` samples from `source` into the ring.
 * `stride` lets the producer pull a single channel out of an interleaved buffer
 * (`stride = channels`); pass `1` for non-interleaved input. Always succeeds —
 * if the consumer is too far behind the oldest unread data is silently
 * overwritten.
 *
 * @param ring Pointer to the ring buffer.
 * @param source Pointer to the source array.
 * @param count Number of samples to write.
 * @param stride Stride for reading from source.
 */
void spsc_audio_ring_buffer_write(spsc_audio_ring_buffer_t* ring,
                                  const float* source, size_t count,
                                  size_t stride);

/**
 * @brief Convert and write double samples as float into the ring buffer.
 *
 * **Producer-only.** Convert `count` `double` samples from `source` to `float`
 * and write into the ring. Used by the spectrum-analyzer tap, which feeds
 * engine-precision `double` samples into a half-precision ring to halve memory.
 *
 * @param ring Pointer to the ring buffer.
 * @param source Pointer to the double source array.
 * @param count Number of samples to convert and write.
 */
void spsc_audio_ring_buffer_append_converting_double_to_float(
    spsc_audio_ring_buffer_t* ring, const double* source, size_t count);

/**
 * @brief Write silence (zeros) into the ring buffer.
 *
 * **Producer-only.** Write `count` zeros into the ring.
 * Always succeeds — if the consumer is too far behind the oldest
 * unread data is silently overwritten.
 *
 * @param ring Pointer to the ring buffer.
 * @param count Number of silent samples to write.
 */
void spsc_audio_ring_buffer_write_silence(spsc_audio_ring_buffer_t* ring,
                                          size_t count);

// MARK: Consumer (consume style)

/**
 * @brief Consume samples from the ring buffer.
 *
 * **Consumer-only.** Copy up to `count` samples into `dest` and advance the
 * read cursor. Returns the number of samples actually copied — may be less than
 * `count` if fewer are available, in which case the remainder of `dest` is left
 * untouched and the caller should fill it with silence.
 *
 * @param ring Pointer to the ring buffer.
 * @param dest Destination buffer.
 * @param count Maximum number of samples to consume.
 * @return Number of samples actually consumed.
 */
size_t spsc_audio_ring_buffer_consume(spsc_audio_ring_buffer_t* ring,
                                      float* dest, size_t count);

/**
 * @brief Consume samples from the ring buffer with a destination stride.
 *
 * **Consumer-only.** Copy up to `count` samples into `dest` with specified
 * `stride` and advance the read cursor. Returns the number of samples actually
 * copied.
 *
 * @param ring Pointer to the ring buffer.
 * @param dest Destination buffer.
 * @param count Maximum number of samples to consume.
 * @param stride Stride for writing to destination.
 * @return Number of samples actually consumed.
 */
size_t spsc_audio_ring_buffer_consume_stride(spsc_audio_ring_buffer_t* ring,
                                             float* dest, size_t count,
                                             size_t stride);

/**
 * @brief Discard all pending samples.
 *
 * **Consumer-only.** Discard any pending samples without copying. Useful when
 * the consumer wants to re-sync after a long stall.
 *
 * @param ring Pointer to the ring buffer.
 */
void spsc_audio_ring_buffer_drain(spsc_audio_ring_buffer_t* ring);

// MARK: Consumer (snapshot style)

/**
 * @brief Read the latest samples without advancing the read cursor.
 *
 * **Consumer.** Copy the most recent `count` samples into `dest` *without*
 * advancing any cursor — subsequent calls can re-read overlapping windows.
 * Returns `false` (without writing to `dest`) when fewer than `count` samples
 * have been written so far.
 *
 * Tearing: in principle the producer can wrap the entire buffer during the
 * consumer's memcpy. With the spectrum analyzer's large buffer, the snapshot
 * is effectively atomic and we don't pay for a seqlock retry loop.
 *
 * @param ring Pointer to the ring buffer.
 * @param dest Destination buffer.
 * @param count Number of samples to read.
 * @return true if successful, false if not enough data.
 */
bool spsc_audio_ring_buffer_read_latest(const spsc_audio_ring_buffer_t* ring,
                                        float* dest, size_t count);

/**
 * @brief Read the latest samples relative to a write index.
 *
 * **Consumer.** Similar to @ref spsc_audio_ring_buffer_read_latest, but reads
 * relative to a specific `written` count.
 *
 * @param ring Pointer to the ring buffer.
 * @param dest Destination buffer.
 * @param count Number of samples to read.
 * @param written The write index offset to read from.
 * @return true if successful, false if not enough data.
 */
bool spsc_audio_ring_buffer_read_latest_at(const spsc_audio_ring_buffer_t* ring,
                                           float* dest, size_t count,
                                           uint64_t written);

/**
 * @brief Round up a size to the next power of two.
 *
 * @param n Size to round.
 * @return Rounded size.
 */
static inline size_t spsc_audio_ring_buffer_round_up_to_power_of_two(size_t n) {
  if (n == 0) return 1;
  if (n > ((size_t)1 << (sizeof(size_t) * 8 - 1))) {
    return (size_t)1 << (sizeof(size_t) * 8 - 1); // Cap at max power of two
  }
  n--;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
#if UINTPTR_MAX == 0xffffffffffffffff
  n |= n >> 32;
#endif
  return n + 1;
}

// MARK: - SPSCQueue

/**
 * @struct spsc_queue
 * @brief Lock-free single-producer / single-consumer FIFO queue of pointers.
 *
 * Used to pass audio chunk values between the capture, processing, and playback
 * threads inside the DSP engine without taking mutexes or locks.
 */
typedef struct spsc_queue spsc_queue_t;

/**
 * @brief Create a new SPSC queue.
 *
 * @param minimum_capacity Minimum requested capacity.
 * @return Pointer to the queue, or NULL on failure.
 */
spsc_queue_t* spsc_queue_create(size_t minimum_capacity);

/**
 * @brief Free the SPSC queue.
 *
 * @param queue Pointer to the queue to free.
 */
void spsc_queue_free(spsc_queue_t* queue);

/**
 * @brief Get the approximate number of queued items.
 *
 * Approximate when read from a thread that is neither producer nor consumer.
 *
 * @param queue Pointer to the queue.
 * @return Number of items in the queue.
 */
size_t spsc_queue_get_count(const spsc_queue_t* queue);

/**
 * @brief Get the capacity of the queue.
 *
 * @param queue Pointer to the queue.
 * @return The capacity.
 */
size_t spsc_queue_get_capacity(const spsc_queue_t* queue);

/**
 * @brief Enqueue a value.
 *
 * **Producer-only.** Append `value`; returns `false` (without storing it)
 * when the queue is at capacity.
 *
 * @param queue Pointer to the queue.
 * @param value The pointer to enqueue.
 * @return true if successful, false if queue is full.
 */
bool spsc_queue_enqueue(spsc_queue_t* queue, void* value);

/**
 * @brief Dequeue a value.
 *
 * **Consumer-only.** Pop the next item; returns NULL when empty.
 *
 * @param queue Pointer to the queue.
 * @return The dequeued pointer, or NULL if empty.
 */
void* spsc_queue_dequeue(spsc_queue_t* queue);

/**
 * @brief Discard all queued items.
 *
 * **Consumer-only.**
 *
 * @param queue Pointer to the queue.
 */
void spsc_queue_drain(spsc_queue_t* queue);

// MARK: - AtomicDouble

/**
 * @struct atomic_double
 * @brief Lock-free atomic `double`.
 *
 * Standard C atomic types don't directly support atomic operations on
 * floating-point types on all targets without locking, so we round-trip through
 * the IEEE-754 bit pattern via `_Atomic uint64_t`.
 */
typedef struct {
  _Atomic uint64_t bits; /**< Atomic bits storing the double representation. */
} atomic_double_t;

/**
 * @brief Initialize an atomic double.
 *
 * @param a Pointer to the atomic double.
 * @param value Initial value.
 */
static inline void atomic_double_init(atomic_double_t* a, double value) {
  uint64_t u;
  memcpy(&u, &value, sizeof(uint64_t));
  atomic_init(&a->bits, u);
}

/**
 * @brief Load an atomic double with explicit memory order.
 *
 * @param a Pointer to the atomic double.
 * @param order Memory order.
 * @return Loaded value.
 */
static inline double atomic_double_load(const atomic_double_t* a,
                                        memory_order order) {
  uint64_t u = atomic_load_explicit(&a->bits, order);
  double d;
  memcpy(&d, &u, sizeof(double));
  return d;
}

/**
 * @brief Store an atomic double with explicit memory order.
 *
 * @param a Pointer to the atomic double.
 * @param value Value to store.
 * @param order Memory order.
 */
static inline void atomic_double_store(atomic_double_t* a, double value,
                                       memory_order order) {
  uint64_t u;
  memcpy(&u, &value, sizeof(uint64_t));
  atomic_store_explicit(&a->bits, u, order);
}

/**
 * @brief Load an atomic double with acquire memory order.
 *
 * @param a Pointer to the atomic double.
 * @return Loaded value.
 */
static inline double atomic_double_get(const atomic_double_t* a) {
  return atomic_double_load(a, memory_order_acquire);
}

/**
 * @brief Store an atomic double with release memory order.
 *
 * @param a Pointer to the atomic double.
 * @param value Value to store.
 */
static inline void atomic_double_set(atomic_double_t* a, double value) {
  atomic_double_store(a, value, memory_order_release);
}

/**
 * @brief Create an atomic double.
 *
 * @param value Initial value.
 * @return Pointer to the allocated atomic double, or NULL on failure.
 */
atomic_double_t* atomic_double_create(double value);

/**
 * @brief Free an atomic double.
 *
 * @param a Pointer to the atomic double to free.
 */
void atomic_double_free(atomic_double_t* a);

#endif  // CLIB_AUDIO_LOCK_FREE_RING_BUFFER_H
