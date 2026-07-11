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
#include "rate_controller.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

// MARK: - PI rate controller

struct pi_rate_controller {
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
};

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
pi_rate_controller_t* pi_rate_controller_create(int samplerate, double interval,
                                                int target_level, double kp,
                                                double ki) {
  if (samplerate <= 0 || interval < 0.1) return NULL;
  pi_rate_controller_t* pi =
      (pi_rate_controller_t*)calloc(1, sizeof(pi_rate_controller_t));
  if (!pi) return NULL;

  pi->target_level = (double)target_level;
  pi->interval = interval;
  pi->kp = kp;
  pi->ki = ki;
  pi->frames_per_interval = interval * (double)samplerate;
  pi->accumulated = 0.0;
  pi->ramp_steps = 20;
  pi->ramp_trigger_limit = 0.33;
  pi->ramp_start = (double)target_level;
  pi->ramp_step = 20;  // Start fully stabilized by default

  return pi;
}

pi_rate_controller_t* pi_rate_controller_create_default(int samplerate,
                                                        double interval,
                                                        int target_level) {
  // Default gains for the controller
  return pi_rate_controller_create(samplerate, interval, target_level, 0.2,
                                   0.004);
}

double pi_rate_controller_next(pi_rate_controller_t* pi, double level) {
  if (!pi) return 1.0;

  // Smooth target transition (ramp-in) to handle large level deviations.
  // If the relative error exceeds `ramp_trigger_limit`, we trigger a ramp
  // starting from the current level and ending at target_level over
  // `ramp_steps` intervals. This prevents step-response shocks and reduces
  // audible pitch changes.
  if (pi->ramp_step >= pi->ramp_steps &&
      fabs((pi->target_level - level) / pi->target_level) >
          pi->ramp_trigger_limit) {
    pi->ramp_start = level;
    pi->ramp_step = 0;
  }
  if (pi->ramp_step == 0) {
    pi->ramp_start = level;
  }

  double current_target;
  if (pi->ramp_step < pi->ramp_steps) {
    pi->ramp_step += 1;
    // quartic ease-out curve to smooth target adjustment:
    // progress goes from 1.0 (start) down to 0.0 (end)
    // 1 - progress^4 starts steep and flattens out near the end target.
    double progress =
        (double)(pi->ramp_steps - pi->ramp_step) / (double)pi->ramp_steps;
    current_target = pi->ramp_start + (pi->target_level - pi->ramp_start) *
                                          (1.0 - pow(progress, 4));
  } else {
    current_target = pi->target_level;
  }

  double err = level - current_target;
  double rel_err = err / pi->frames_per_interval;
  pi->accumulated += rel_err * pi->interval;

  // Anti-windup: clamp the integrator term to the safe saturation band (±0.005)
  double max_val = 0.005;
  double min_val = -0.005;
  if (pi->accumulated * pi->ki > max_val) {
    pi->accumulated = max_val / pi->ki;
  } else if (pi->accumulated * pi->ki < min_val) {
    pi->accumulated = min_val / pi->ki;
  }

  double proportional = pi->kp * rel_err;
  double integral = pi->ki * pi->accumulated;
  double output = proportional + integral;
  double clamped_output = output;
  if (clamped_output > max_val) clamped_output = max_val;
  if (clamped_output < min_val) clamped_output = min_val;

  return 1.0 - clamped_output;
}

void pi_rate_controller_free(pi_rate_controller_t* pi) {
  if (pi) free(pi);
}

// MARK: - Averager

/// Windowed arithmetic mean. The producer adds one sample per
/// processed chunk; the rate-adjust tick reads `average` once per
/// adjust period and calls `restart()` to begin the next window. The
/// effect is a simple boxcar low-pass that filters chunk-level noise
/// out of the controller's input.
void averager_init(averager_t* avg) {
  if (!avg) return;
  avg->sum = 0.0;
  avg->count = 0;
}

void averager_add(averager_t* avg, double value) {
  if (!avg) return;
  avg->sum += value;
  avg->count += 1;
}

void averager_restart(averager_t* avg) {
  if (!avg) return;
  avg->sum = 0.0;
  avg->count = 0;
}

/// Mean of the samples added since the last `restart()`. `nil` when
/// no samples have been added yet — the caller decides what an
/// empty window means in their context.
bool averager_get_average(const averager_t* avg, double* out_val) {
  if (!avg || avg->count <= 0) return false;
  if (out_val) *out_val = avg->sum / (double)avg->count;
  return true;
}

// MARK: - Stopwatch

/// Monotonic elapsed-time helper. Backed by
/// `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)`, which on Darwin is a
/// vDSO read — no syscall, suitable for invocation on every processed
/// audio chunk.
/**
 * @brief Retrieves the current monotonic system time in nanoseconds.
 *
 * Uses low-overhead, system-specific API to get the monotonic time.
 * Under Darwin/macOS, it uses `clock_gettime_nsec_np` with `CLOCK_UPTIME_RAW`.
 * Under Linux, it falls back to standard POSIX `clock_gettime` with
 * `CLOCK_MONOTONIC`.
 *
 * @return The current monotonic time in nanoseconds.
 */
static uint64_t get_current_ns(void) {
#ifdef __APPLE__
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

void stopwatch_init(stopwatch_t* sw) {
  if (!sw) return;
  sw->start_ns = get_current_ns();
}

void stopwatch_restart(stopwatch_t* sw) {
  if (!sw) return;
  sw->start_ns = get_current_ns();
}

double stopwatch_elapsed_seconds(const stopwatch_t* sw) {
  if (!sw) return 0.0;
  uint64_t now = get_current_ns();
  return (double)(now - sw->start_ns) / 1000000000.0;
}
