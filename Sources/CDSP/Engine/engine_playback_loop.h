#ifndef CLIB_ENGINE_ENGINE_PLAYBACK_LOOP_H
#define CLIB_ENGINE_ENGINE_PLAYBACK_LOOP_H

/**
 * @file engine_playback_loop.h
 * @brief Playback thread loop for the DSP engine.
 *
 * Drains the processing→playback SPSC queue and writes each chunk to the
 * playback backend. Also runs the rate-adjust control loop: averages the
 * (device-ring + queued-chunks) fill level, and once per `adjustPeriod` seconds
 * feeds the average to `PIRateController`.
 *
 * @section state_ownership State ownership
 * The rate-adjust state — controller, averager, stopwatch, last
 * published speed — is local to this loop. The output speed is
 * applied either directly to the capture clock (when the capture
 * device exposes a tunable clock — BlackHole 0.5.0+) or published
 * via `shared.resamplerRatio` so the processing thread picks it up
 * on its next chunk.
 *
 * @section audio_invariants Audio-thread invariants
 * - No allocations in the steady state. The controller and
 *   averager are constructed once at init; the stopwatch is a
 *   plain UInt64 nanosecond timestamp.
 * - No locks. The shared SPSC queue + semaphore carries chunks
 *   and wakeups.
 * - The rate-adjust info logger fires at most once per
 *   `adjustPeriod` (~10 s default), so its formatting cost is
 *   negligible per chunk.
 */

#include <stdbool.h>
#include <stddef.h>

#include "Audio/processing_parameters.h"
#include "Backend/audio_backend.h"
#include "engine_shared_state.h"
#include "engine_state_machine.h"
#include "rate_controller.h"

/**
 * @brief Opaque structure representing the playback loop.
 *
 * `@unchecked Sendable` is a *transfer* vouch, not a *share*
 * vouch: the instance is safe to cross the Thread spawn boundary
 * because exactly one thread (the loop thread) ever touches it
 * after `run()` is invoked. The rate-adjust controller, averager,
 * and stopwatch are all loop-local state with no synchronisation
 * and are *not* safe to use from multiple threads concurrently.
 */
typedef struct engine_playback_loop engine_playback_loop_t;

/**
 * @brief Creates a new engine playback loop instance.
 *
 * @param shared Pointer to the shared state.
 * @param capture Pointer to the capture backend.
 * @param playback Pointer to the playback backend.
 * @param processing_params Pointer to the processing parameters.
 * @param pipeline_rate Rate of the pipeline.
 * @param chunk_size Size of each audio chunk in frames.
 * @param rate_adjust_enabled Whether rate adjustment is enabled.
 * @param adjust_period Interval for rate adjustment calculation in seconds.
 * @param target_level Target buffer level.
 * @return Pointer to the created playback loop instance, or NULL on failure.
 */
engine_playback_loop_t* engine_playback_loop_create(
    engine_shared_state_t* shared, capture_backend_t* capture,
    playback_backend_t* playback, processing_parameters_t* processing_params,
    size_t pipeline_rate, size_t chunk_size, bool rate_adjust_enabled,
    double adjust_period, int target_level);

/**
 * @brief Frees the engine playback loop instance.
 *
 * @param loop Pointer to the playback loop instance to free.
 */
void engine_playback_loop_free(engine_playback_loop_t* loop);

/**
 * @brief Runs the playback loop.
 *
 * This function blocks and runs the playback loop until it is requested to stop
 * or an error occurs.
 *
 * @param loop Pointer to the playback loop instance.
 */
void engine_playback_loop_run(engine_playback_loop_t* loop);

#endif  // CLIB_ENGINE_ENGINE_PLAYBACK_LOOP_H
