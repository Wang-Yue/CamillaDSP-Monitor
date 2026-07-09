#ifndef CLIB_ENGINE_SAMPLE_RATE_WATCHER_H
#define CLIB_ENGINE_SAMPLE_RATE_WATCHER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Opaque structure representing the sample rate watcher.
 */
typedef struct sample_rate_watcher sample_rate_watcher_t;

/**
 * @brief Creates a new sample rate watcher instance.
 */
sample_rate_watcher_t* sample_rate_watcher_create(double target_rate,
                                                  double measure_interval,
                                                  bool stop_on_rate_change);

/**
 * @brief Frees the sample rate watcher instance.
 */
void sample_rate_watcher_free(sample_rate_watcher_t* watcher);

/**
 * @brief Resets the rate watcher statistics and timers.
 */
void sample_rate_watcher_reset(sample_rate_watcher_t* watcher);

/**
 * @brief Records a chunk of size `frames` and checks for rate change deviation.
 *
 * @param watcher Pointer to the sample rate watcher.
 * @param frames Number of frames read in the chunk.
 * @param out_measured_rate Pointer to double to receive the measured rate if a
 * change is detected.
 * @return true if a sample rate change was detected, false otherwise.
 */
bool sample_rate_watcher_tick(sample_rate_watcher_t* watcher, size_t frames,
                              double* out_measured_rate);

/**
 * @brief Gets whether the watcher is configured to stop on rate changes.
 */
bool sample_rate_watcher_get_stop_on_rate_change(
    const sample_rate_watcher_t* watcher);

#endif  // CLIB_ENGINE_SAMPLE_RATE_WATCHER_H
