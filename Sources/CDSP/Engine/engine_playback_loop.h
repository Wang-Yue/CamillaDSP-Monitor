#ifndef CLIB_ENGINE_ENGINE_PLAYBACK_LOOP_H
#define CLIB_ENGINE_ENGINE_PLAYBACK_LOOP_H

// Playback thread body. Drains the processing→playback SPSC queue
// and writes each chunk to the playback backend. Also runs the
// rate-adjust control loop: averages the (device-ring + queued-chunks)
// fill level, and once per `adjustPeriod` seconds feeds the average
// to `PIRateController`.
//
// State ownership
// ---------------
// The rate-adjust state — controller, averager, stopwatch, last
// published speed — is local to this loop. The output speed is
// applied either directly to the capture clock (when the capture
// device exposes a tunable clock — BlackHole 0.5.0+) or published
// via `shared.resamplerRatio` so the processing thread picks it up
// on its next chunk.
//
// Audio-thread invariants
// -----------------------
//   * No allocations in the steady state. The controller and
//     averager are constructed once at init; the stopwatch is a
//     plain UInt64 nanosecond timestamp.
//   * No locks. The shared SPSC queue + semaphore carries chunks
//     and wakeups.
//   * The rate-adjust info logger fires at most once per
//     `adjustPeriod` (~10 s default), so its formatting cost is
//     negligible per chunk.

#include "engine_shared_state.h"
#include "engine_state_machine.h"
#include "Backend/audio_backend.h"
#include "rate_controller.h"
#include "Audio/processing_parameters.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


/// `@unchecked Sendable` is a *transfer* vouch, not a *share*
/// vouch: the instance is safe to cross the Thread spawn boundary
/// because exactly one thread (the loop thread) ever touches it
/// after `run()` is invoked. The rate-adjust controller, averager,
/// and stopwatch are all loop-local state with no synchronisation
/// and are *not* safe to use from multiple threads concurrently.
typedef struct {
    engine_shared_state_t* shared;
    capture_backend_t* capture;
    playback_backend_t* playback;
    processing_parameters_t* processing_params;
    size_t pipeline_rate;
    size_t chunk_size;
    bool pitch_supported;
    bool rate_adjust_enabled;
    double adjust_period;
    int target_level;
    engine_stop_callback_t on_stop;
    void* on_stop_ctx;
} engine_playback_loop_t;

engine_playback_loop_t* engine_playback_loop_create(
    engine_shared_state_t* shared,
    capture_backend_t* capture,
    playback_backend_t* playback,
    processing_parameters_t* processing_params,
    size_t pipeline_rate,
    size_t chunk_size,
    bool rate_adjust_enabled,
    double adjust_period,
    int target_level,
    engine_stop_callback_t on_stop,
    void* on_stop_ctx
);

void engine_playback_loop_free(engine_playback_loop_t* loop);
void engine_playback_loop_run(engine_playback_loop_t* loop);

#ifdef __cplusplus
}
#endif

#endif // CLIB_ENGINE_ENGINE_PLAYBACK_LOOP_H
