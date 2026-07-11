#ifndef CLIB_ENGINE_ENGINE_SHARED_STATE_H
#define CLIB_ENGINE_ENGINE_SHARED_STATE_H

/**
 * @file engine_shared_state.h
 * @brief Inter-thread state for the DSP engine's audio-priority loops.
 *
 * Coordinates state between the capture, processing, and playback loops.
 * Every field is either a lock-free atomic, a wait-free SPSC queue, or a kernel
 * signaling primitive (`DispatchSemaphore`/semaphore/Event). No mutexes are
 * used, allowing the loops to read/write fields without coordinating locks.
 *
 * @section concurrency_model Concurrency model
 * - `should_stop`: Written by `stop()`, read by all three loops every
 * iteration. Uses release-acquire atomic semantics.
 * - `captured_queue`: SPSC, producer = capture, consumer = processing.
 * - `processed_queue`: SPSC, producer = processing, consumer = playback.
 * - `captured_semaphore`: Capture signals, processing waits.
 * - `processed_semaphore`: Processing signals, playback waits.
 * - `resampler_ratio`: Playback writes (rate-adjust), processing reads (per
 * chunk). 64-bit atomic.
 *
 * @section semaphores Semaphores
 * Semaphores are used for kernel-level signaling (not locking). Producers
 * signal after enqueueing, and consumers wait then drain.
 */

#include "Audio/audio_chunk.h"
#include "Audio/lock_free_ring_buffer.h"
#ifdef __APPLE__
#include <dispatch/dispatch.h>
/**
 * @brief Platform-specific semaphore wrapper.
 */
typedef dispatch_semaphore_t engine_semaphore_t;
/**
 * @brief Initializes the semaphore.
 * @param sem Pointer to the semaphore wrapper.
 * @return true on success, false on failure.
 */
static inline bool engine_sem_init(engine_semaphore_t* sem) {
  *sem = dispatch_semaphore_create(0);
  return *sem != NULL;
}
/**
 * @brief Destroys the semaphore.
 * @param sem Pointer to the semaphore wrapper.
 */
static inline void engine_sem_destroy(engine_semaphore_t* sem) {
  if (*sem) dispatch_release(*sem);
}
/**
 * @brief Signals the semaphore.
 * @param sem The semaphore.
 */
static inline void engine_sem_signal(engine_semaphore_t sem) {
  if (sem) dispatch_semaphore_signal(sem);
}
/**
 * @brief Waits on the semaphore.
 * @param sem The semaphore.
 */
static inline void engine_sem_wait(engine_semaphore_t sem) {
  if (sem) dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
}
#elif defined(__linux__)
#include <semaphore.h>
#include <stdlib.h>
/**
 * @brief Platform-specific semaphore wrapper.
 */
typedef sem_t* engine_semaphore_t;
/**
 * @brief Initializes the semaphore.
 * @param sem Pointer to the semaphore wrapper.
 * @return true on success, false on failure.
 */
static inline bool engine_sem_init(engine_semaphore_t* sem) {
  *sem = (sem_t*)malloc(sizeof(sem_t));
  if (!*sem) return false;
  if (sem_init(*sem, 0, 0) != 0) {
    free(*sem);
    *sem = NULL;
    return false;
  }
  return true;
}
/**
 * @brief Destroys the semaphore.
 * @param sem Pointer to the semaphore wrapper.
 */
static inline void engine_sem_destroy(engine_semaphore_t* sem) {
  if (*sem) {
    sem_destroy(*sem);
    free(*sem);
    *sem = NULL;
  }
}
/**
 * @brief Signals the semaphore.
 * @param sem The semaphore.
 */
static inline void engine_sem_signal(engine_semaphore_t sem) {
  if (sem) sem_post(sem);
}
/**
 * @brief Waits on the semaphore.
 * @param sem The semaphore.
 */
static inline void engine_sem_wait(engine_semaphore_t sem) {
  if (sem) sem_wait(sem);
}
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/**
 * @brief Platform-specific semaphore wrapper.
 */
typedef HANDLE engine_semaphore_t;
/**
 * @brief Initializes the semaphore.
 * @param sem Pointer to the semaphore wrapper.
 * @return true on success, false on failure.
 */
static inline bool engine_sem_init(engine_semaphore_t* sem) {
  *sem = CreateSemaphore(NULL, 0, 32767, NULL);
  return *sem != NULL;
}
/**
 * @brief Destroys the semaphore.
 * @param sem Pointer to the semaphore wrapper.
 */
static inline void engine_sem_destroy(engine_semaphore_t* sem) {
  if (*sem) {
    CloseHandle(*sem);
    *sem = NULL;
  }
}
/**
 * @brief Signals the semaphore.
 * @param sem The semaphore.
 */
static inline void engine_sem_signal(engine_semaphore_t sem) {
  if (sem) ReleaseSemaphore(sem, 1, NULL);
}
/**
 * @brief Waits on the semaphore.
 * @param sem The semaphore.
 */
static inline void engine_sem_wait(engine_semaphore_t sem) {
  if (sem) WaitForSingleObject(sem, INFINITE);
}
#endif

/**
 * @brief Yields the current thread's CPU execution slice.
 *
 * Yields execution to another thread that is ready to run on the current processor.
 * Maps to sched_yield() on POSIX (Linux/macOS) and SwitchToThread() on Windows.
 * Used to propagate queue backpressure without forcing a minimum sleep duration.
 */
#if defined(__APPLE__) || defined(__linux__)
#include <sched.h>
static inline void engine_yield(void) {
  sched_yield();
}
#elif defined(_WIN32)
static inline void engine_yield(void) {
  SwitchToThread();
}
#endif
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "Config/engine_config_types.h"

/**
 * @brief Shared state between the engine threads.
 *
 * Genuinely `Sendable` — every stored field is itself `Sendable`
 * (the SPSC queues, the kernel semaphores, and the atomics).
 * Producers and consumers may freely access these from any thread
 * without coordination beyond what each individual field's API requires.
 */
typedef struct {
  /**
   * @brief Bounded SPSC FIFO from the capture thread to the processing thread.
   * `enqueue` returns `false` when full; the producer drops the chunk rather
   * than allocate.
   */
  spsc_queue_t* captured_queue;

  /**
   * @brief Bounded SPSC FIFO from the processing thread to the playback thread.
   */
  spsc_queue_t* processed_queue;

  /**
   * @brief Wakeup signal for the processing thread.
   * The capture thread signals after every successful `enqueue`.
   */
  engine_semaphore_t captured_semaphore;

  /**
   * @brief Wakeup signal for the playback thread.
   * The processing thread signals after every successful `enqueue`.
   */
  engine_semaphore_t processed_semaphore;

  /**
   * @brief Stop flag.
   * Written exactly once (false → true) per engine run.
   * Each loop polls between iterations and exits when set.
   */
  _Atomic bool should_stop;

  /**
   * @brief Guard flag to ensure stop_reason is only written once (no
   * write-write race).
   */
  _Atomic bool stop_reason_written;

  /**
   * @brief Pipeline status flags to propagate graceful EOF sequentially.
   */
  _Atomic bool capture_finished;
  _Atomic bool processing_finished;

  /**
   * @brief Stop reason explaining why the loop stopped (e.g. format change).
   */
  processing_stop_reason_t stop_reason;

  /**
   * @brief Resampler relative-ratio (≈ 1.0).
   * Published by the playback thread (rate-adjust controller); consumed by
   * the processing thread once per chunk via `setRelativeRatio`.
   */
  atomic_double_t* resampler_ratio;

  /**
   * @brief Deferred free queue for old pipeline structures.
   *
   * Holds pipeline instances swapped out by the processing thread. The control
   * thread periodically dequeues and frees them asynchronously to keep the
   * audio thread allocation-free and real-time safe.
   */
  spsc_queue_t* pipeline_garbage_queue;
} engine_shared_state_t;

/**
 * @brief Creates a new engine shared state instance.
 *
 * @param captured_queue_depth Depth of the captured SPSC queue.
 * @param processed_queue_depth Depth of the processed SPSC queue.
 * @return Pointer to the created shared state instance, or NULL on failure.
 */
engine_shared_state_t* engine_shared_state_create(size_t captured_queue_depth,
                                                  size_t processed_queue_depth);

/**
 * @brief Frees the engine shared state instance.
 *
 * @param state Pointer to the shared state instance to free.
 */
void engine_shared_state_free(engine_shared_state_t* state);

/**
 * @brief Requests the engine loops to stop.
 *
 * Sets the `should_stop` flag and sets the stop reason.
 *
 * @param state Pointer to the shared state.
 * @param reason The reason for stopping.
 */
void engine_shared_state_request_stop(engine_shared_state_t* state,
                                      processing_stop_reason_t reason);

#endif  // CLIB_ENGINE_ENGINE_SHARED_STATE_H
