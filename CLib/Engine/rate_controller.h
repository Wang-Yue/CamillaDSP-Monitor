#ifndef CLIB_ENGINE_RATE_CONTROLLER_H
#define CLIB_ENGINE_RATE_CONTROLLER_H

// Drift-compensation primitives used by the engine's rate-adjust loop.
// Clean-room implementation grounded in standard control-theory practice
// — the algorithms are textbook discrete-time PI with output saturation
// and integrator clamping for anti-windup. No code lineage from any
// other audio project.
//
// References:
//   * K. J. Åström, R. M. Murray, "Feedback Systems: An Introduction
//     for Scientists and Engineers" (Princeton UP, 2008), §10 on PID
//     and §11 on integrator anti-windup.
//   * A. V. Oppenheim, R. W. Schafer, "Discrete-Time Signal
//     Processing" (Prentice Hall), §3 on difference equations — the
//     digital integrator is the canonical accumulator.
//
// Plant model (rate-adjust as a feedback control problem)
// -------------------------------------------------------
// The "level" we observe is the playback ring-buffer fill in samples.
// If the capture clock runs at `Fs · (1 + u)` samples per second and
// the playback clock at `Fs · (1 + δ)` for some unknown small drift
// `δ`, the buffer fill `L(t)` satisfies
//
//     dL/dt = Fs · (u − δ).
//
// In the Laplace domain that's an integrator with DC gain `Fs`. A
// proportional-integral controller in series gives a 2-pole closed
// loop whose characteristic polynomial is
//
//     s² + Fs·Kp · s + Fs·Ki  =  s² + 2ζωn s + ωn²,
//
// from which `Kp = 2ζωn / Fs` and `Ki = ωn² / Fs`. Picking `ωn` and
// `ζ` directly is a more honest way to tune than groping for raw
// gains, so the convenience initializer takes that route.

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// MARK: - PI rate controller

/// Discrete-time proportional-integral controller that produces a
/// speed multiplier `≈ 1.0` from a measured buffer-level sample. The
/// output is intended to be applied multiplicatively to the capture
/// clock (when the device exposes a tunable clock) or to the
/// resampler's relative ratio (otherwise).
///
/// **Sign convention.** `e = setpoint − level`. A buffer that is too
/// low (capture is running too slowly relative to playback) gives a
/// positive error and yields `speed > 1`, asking the capture path to
/// run a touch faster. A buffer that is too full does the opposite.
///
/// **Saturation.** The output is hard-limited to `1 ± maxAdjustment`
/// so a single tick is always inaudible. The integrator state is
/// clamped to the same band — this is the standard
/// conditional-integration form of anti-windup, which prevents the
/// integrator from accumulating during sustained saturation.
typedef struct {
    double target_level;
    double interval;
    double kp;
    double ki;
    double frames_per_interval;
    double accumulated;
    int ramp_steps;
    double ramp_trigger_limit;
    double ramp_start;
    int ramp_step;
} pi_rate_controller_t;

pi_rate_controller_t* pi_rate_controller_create_default(int samplerate, double interval, int target_level);
pi_rate_controller_t* pi_rate_controller_create(int samplerate, double interval, int target_level, double kp, double ki);
double pi_rate_controller_next(pi_rate_controller_t* pi, double level);
void pi_rate_controller_free(pi_rate_controller_t* pi);

// MARK: - Averager

/// Windowed arithmetic mean. The producer adds one sample per
/// processed chunk; the rate-adjust tick reads `average` once per
/// adjust period and calls `restart()` to begin the next window. The
/// effect is a simple boxcar low-pass that filters chunk-level noise
/// out of the controller's input.
typedef struct {
    double sum;
    int count;
} averager_t;

void averager_init(averager_t* avg);
void averager_add(averager_t* avg, double value);
void averager_restart(averager_t* avg);

/// Mean of the samples added since the last `restart()`. `nil` when
/// no samples have been added yet — the caller decides what an
/// empty window means in their context.
bool averager_get_average(const averager_t* avg, double* out_val);

// MARK: - Stopwatch

/// Monotonic elapsed-time helper. Backed by
/// `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)`, which on Darwin is a
/// vDSO read — no syscall, suitable for invocation on every processed
/// audio chunk.
typedef struct {
    uint64_t start_ns;
} stopwatch_t;

void stopwatch_init(stopwatch_t* sw);
void stopwatch_restart(stopwatch_t* sw);
double stopwatch_elapsed_seconds(const stopwatch_t* sw);

#ifdef __cplusplus
}
#endif

#endif // CLIB_ENGINE_RATE_CONTROLLER_H
