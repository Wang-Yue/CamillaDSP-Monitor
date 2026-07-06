// Single-producer / single-consumer lock-free primitives used by the
// audio thread. Two shapes:
//
//   * `spsc_audio_ring_buffer_t` — power-of-two ring of `float` samples with
//     two access patterns:
//       - **Consume style** (`write` + `consume`): the consumer
//         advances a read cursor; each sample is delivered exactly
//         once. Used by the audio capture and playback paths.
//       - **Snapshot style** (`append_converting_double_to_float` +
//         `read_latest`): the consumer takes the most-recent N
//         samples without advancing any cursor; the same window
//         can be re-read for FFTs at different lengths. Used by
//         `spectrum_analyzer_t`.
//     The two patterns share the same producer index, so a single
//     ring can serve either role — they don't mix on a given ring,
//     but the same primitive covers both.
//
//   * `spsc_queue_t` — generic SPSC FIFO queue (storing `void*` pointers).
//     Used to pass audio chunk values between the capture, processing, and
//     playback threads inside the DSP engine without taking mutexes or locks.
//
//   * `atomic_double_t` — a wait-free `double` atom built on
//     `_Atomic uint64_t` round-tripped through the IEEE-754 bit pattern.
//     Used by the rate-adjust loop to publish the resampler ratio
//     from the playback thread to the processing thread.
//
// Real-time discipline
// --------------------
// All hot-path methods are wait-free, allocation-free, and free of
// runtime calls or syscalls that could block. The producer always succeeds
// — if the consumer is so far behind that the buffer is full, the
// oldest unread data is silently overwritten (matching the original
// lock-based design's drop-on-overflow behaviour).

#ifndef CLIB_AUDIO_LOCK_FREE_RING_BUFFER_H
#define CLIB_AUDIO_LOCK_FREE_RING_BUFFER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// MARK: - SPSCAudioRingBuffer

/// Lock-free SPSC ring buffer of `float` samples. Power-of-two
/// capacity so wrap-around is a single bitmask. Producer publishes
/// new samples with a `release` store on `write_index`; consumers
/// observe with an `acquire` load, establishing happens-before
/// without locks.
///
/// Two consumer styles, both supported on the same instance — but
/// don't mix them on a single ring:
///
///   - **Consume:** call `spsc_audio_ring_buffer_consume(dest, count)` to drain samples.
///     Each sample is delivered to exactly one consumer call.
///     Used by audio capture and playback paths.
///   - **Snapshot:** call `spsc_audio_ring_buffer_read_latest(dest, count)` to copy the
///     most-recent `count` samples without advancing any cursor.
///     The same samples can be re-read across calls. Used by
///     `spectrum_analyzer_t` to feed FFTs at different lengths.
typedef struct {
    /// Capacity in samples (always a power of two).
    size_t capacity;
    size_t mask;
    float* storage;
    /// Monotonically increasing count of samples written since
    /// allocation. The release-store synchronises with consumers'
    /// acquire-loads, so any reader that observes the new count is
    /// guaranteed to see the corresponding sample writes.
    _Atomic uint64_t write_index;
    /// Monotonic samples drained by the consumer in `consume(...)`.
    /// Only used by the consume-style API; snapshot readers ignore
    /// this entirely.
    _Atomic uint64_t read_index;
} spsc_audio_ring_buffer_t;

spsc_audio_ring_buffer_t* spsc_audio_ring_buffer_create(size_t minimum_capacity);
void spsc_audio_ring_buffer_free(spsc_audio_ring_buffer_t* ring);

/// Total samples written since allocation. Observed with
/// `memory_order_relaxed`; callers that need happens-before with the
/// payload should use `consume` or `read_latest` instead.
static inline uint64_t spsc_audio_ring_buffer_get_total_samples_written(const spsc_audio_ring_buffer_t* ring) {
    return atomic_load_explicit((_Atomic uint64_t*)&ring->write_index, memory_order_relaxed);
}

/// Number of samples currently waiting to be consumed (for
/// consume-style use). Always non-negative.
static inline size_t spsc_audio_ring_buffer_get_available_to_read(const spsc_audio_ring_buffer_t* ring) {
    uint64_t w = atomic_load_explicit((_Atomic uint64_t*)&ring->write_index, memory_order_acquire);
    uint64_t r = atomic_load_explicit((_Atomic uint64_t*)&ring->read_index, memory_order_relaxed);
    return (size_t)(w - r);
}

// MARK: Producer

/// **Producer-only.** Write `count` `float` samples from
/// `source` into the ring. `stride` lets the producer pull a
/// single channel out of an interleaved buffer (`stride =
/// channels`); pass `1` for non-interleaved input. Always
/// succeeds — if the consumer is too far behind the oldest
/// unread data is silently overwritten.
void spsc_audio_ring_buffer_write(spsc_audio_ring_buffer_t* ring, const float* source, size_t count, size_t stride);

/// **Producer-only.** Convert `count` `double` samples from
/// `source` to `float` in a single `vDSP_vdpsp` call and write
/// into the ring. Used by the spectrum-analyzer tap, which feeds
/// engine-precision `double` samples into a half-precision ring
/// to halve memory.
void spsc_audio_ring_buffer_append_converting_double_to_float(spsc_audio_ring_buffer_t* ring, const double* source, size_t count);

/// **Producer-only.** Write `count` zeros into the ring.
/// Always succeeds — if the consumer is too far behind the oldest
/// unread data is silently overwritten.
void spsc_audio_ring_buffer_write_silence(spsc_audio_ring_buffer_t* ring, size_t count);

// MARK: Consumer (consume style)

/// **Consumer-only.** Copy up to `count` samples into `dest` and
/// advance the read cursor. Returns the number of samples
/// actually copied — may be less than `count` if fewer are
/// available, in which case the remainder of `dest` is left
/// untouched and the caller should fill it with silence.
size_t spsc_audio_ring_buffer_consume(spsc_audio_ring_buffer_t* ring, float* dest, size_t count);

/// **Consumer-only.** Discard any pending samples without
/// copying. Useful when the consumer wants to re-sync after a
/// long stall.
void spsc_audio_ring_buffer_drain(spsc_audio_ring_buffer_t* ring);

// MARK: Consumer (snapshot style)

/// **Consumer.** Copy the most recent `count` samples into
/// `dest` *without* advancing any cursor — subsequent calls
/// can re-read overlapping windows. Returns `false` (without
/// writing to `dest`) when fewer than `count` samples have been
/// written so far.
///
/// Tearing: in principle the producer can wrap the entire buffer
/// during the consumer's memcpy. With the spectrum analyzer's
/// 262 144-sample buffer at 48 kHz that's ~5.5 s of audio
/// headroom — orders of magnitude longer than the consumer
/// takes — so the snapshot is effectively atomic and we don't
/// pay for a seqlock retry loop.
bool spsc_audio_ring_buffer_read_latest(const spsc_audio_ring_buffer_t* ring, float* dest, size_t count);
bool spsc_audio_ring_buffer_read_latest_at(const spsc_audio_ring_buffer_t* ring, float* dest, size_t count, uint64_t written);

static inline size_t spsc_audio_ring_buffer_round_up_to_power_of_two(size_t n) {
    size_t v = 1;
    while (v < n) {
        v <<= 1;
    }
    return v;
}

// MARK: - SPSCQueue

/// Lock-free single-producer / single-consumer FIFO queue of values of
/// arbitrary type (`void*` pointers). Used to pass audio chunk values between the
/// capture, processing, and playback threads inside the DSP engine
/// without taking mutexes or locks.
///
/// Power-of-two capacity. Slots store `void*` pointers (NULL when empty) so the consumer
/// can clear back to NULL on dequeue.
typedef struct {
    size_t capacity;
    size_t mask;
    /// Slot storage. Each slot holds NULL when empty; the producer
    /// fills it on enqueue and the consumer clears it back to NULL on
    /// dequeue.
    void** storage;
    _Atomic uint64_t write_index;
    _Atomic uint64_t read_index;
} spsc_queue_t;

spsc_queue_t* spsc_queue_create(size_t minimum_capacity);
void spsc_queue_free(spsc_queue_t* queue);

/// Number of currently-queued items. Approximate when read from a
/// thread that is neither the producer nor the consumer.
static inline size_t spsc_queue_get_count(const spsc_queue_t* queue) {
    uint64_t w = atomic_load_explicit((_Atomic uint64_t*)&queue->write_index, memory_order_acquire);
    uint64_t r = atomic_load_explicit((_Atomic uint64_t*)&queue->read_index, memory_order_relaxed);
    return (size_t)(w - r);
}

/// **Producer-only.** Append `value`; returns `false` (without
/// storing it) when the queue is at capacity. The caller decides
/// what to do — drop, log, or retry.
bool spsc_queue_enqueue(spsc_queue_t* queue, void* value);
/// **Consumer-only.** Pop the next item; returns NULL when empty.
void* spsc_queue_dequeue(spsc_queue_t* queue);
/// **Consumer-only.** Discard all queued items.
void spsc_queue_drain(spsc_queue_t* queue);

// MARK: - AtomicDouble

/// Lock-free atomic `double`. Standard C atomic types don't directly support
/// atomic operations on floating-point types on all targets without locking, so we round-trip through the IEEE-754 bit
/// pattern via `_Atomic uint64_t`. Aligned 64-bit loads and stores are atomic on
/// 64-bit architectures (and ARM/x86 targets), so this is genuinely wait-free.
typedef struct {
    _Atomic uint64_t bits;
} atomic_double_t;

static inline void atomic_double_init(atomic_double_t* a, double value) {
    uint64_t u;
    memcpy(&u, &value, sizeof(uint64_t));
    atomic_init(&a->bits, u);
}

static inline double atomic_double_load(const atomic_double_t* a, memory_order order) {
    uint64_t u = atomic_load_explicit((_Atomic uint64_t*)&a->bits, order);
    double d;
    memcpy(&d, &u, sizeof(double));
    return d;
}

static inline void atomic_double_store(atomic_double_t* a, double value, memory_order order) {
    uint64_t u;
    memcpy(&u, &value, sizeof(uint64_t));
    atomic_store_explicit(&a->bits, u, order);
}

static inline double atomic_double_get(const atomic_double_t* a) {
    return atomic_double_load(a, memory_order_acquire);
}

static inline void atomic_double_set(atomic_double_t* a, double value) {
    atomic_double_store(a, value, memory_order_release);
}

atomic_double_t* atomic_double_create(double value);
void atomic_double_free(atomic_double_t* a);

#ifdef __cplusplus
}
#endif

#endif // CLIB_AUDIO_LOCK_FREE_RING_BUFFER_H
