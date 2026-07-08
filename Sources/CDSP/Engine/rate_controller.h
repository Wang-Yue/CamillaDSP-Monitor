#ifndef CLIB_ENGINE_RATE_CONTROLLER_H
#define CLIB_ENGINE_RATE_CONTROLLER_H

/**
 * @file rate_controller.h
 * @brief Drift-compensation primitives used by the engine's rate-adjust loop.
 *
 * Clean-room implementation grounded in standard control-theory practice
 * — the algorithms are textbook discrete-time PI with output saturation
 * and integrator clamping for anti-windup. No code lineage from any
 * other audio project.
 *
 * References:
 *   * K. J. Åström, R. M. Murray, "Feedback Systems: An Introduction
 *     for Scientists and Engineers" (Princeton UP, 2008), §10 on PID
 *     and §11 on integrator anti-windup.
 *   * A. V. Oppenheim, R. W. Schafer, "Discrete-Time Signal
 *     Processing" (Prentice Hall), §3 on difference equations — the
 *     digital integrator is the canonical accumulator.
 *
 * Plant model (rate-adjust as a feedback control problem)
 * -------------------------------------------------------
 * The "level" we observe is the playback ring-buffer fill in samples.
 * If the capture clock runs at `Fs · (1 + u)` samples per second and
 * the playback clock at `Fs · (1 + δ)` for some unknown small drift
 * `δ`, the buffer fill `L(t)` satisfies
 *
 *     dL/dt = Fs · (u − δ).
 *
 * In the Laplace domain that's an integrator with DC gain `Fs`. A
 * proportional-integral controller in series gives a 2-pole closed
 * loop whose characteristic polynomial is
 *
 *     s² + Fs·Kp · s + Fs·Ki  =  s² + 2ζωn s + ωn²,
 *
 * from which `Kp = 2ζωn / Fs` and `Ki = ωn² / Fs`. Picking `ωn` and
 * `ζ` directly is a more honest way to tune than groping for raw
 * gains, so the convenience initializer takes that route.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// MARK: - PI rate controller

/**
 * @brief Discrete-time proportional-integral controller that produces a
 * speed multiplier `≈ 1.0` from a measured buffer-level sample.
 *
 * The output is intended to be applied multiplicatively to the capture
 * clock (when the device exposes a tunable clock) or to the
 * resampler's relative ratio (otherwise).
 *
 * **Sign convention.** `e = setpoint − level`. A buffer that is too
 * low (capture is running too slowly relative to playback) gives a
 * positive error and yields `speed > 1`, asking the capture path to
 * run a touch faster. A buffer that is too full does the opposite.
 *
 * **Saturation.** The output is hard-limited to `1 ± maxAdjustment`
 * so a single tick is always inaudible. The integrator state is
 * clamped to the same band — this is the standard
 * conditional-integration form of anti-windup, which prevents the
 * integrator from accumulating during sustained saturation.
 */
typedef struct pi_rate_controller pi_rate_controller_t;

/**
 * @brief Creates a PI rate controller with default parameters.
 *
 * @param samplerate The sample rate in Hz.
 * @param interval The control interval in seconds.
 * @param target_level The target buffer level in samples.
 * @return A pointer to the newly created pi_rate_controller_t instance, or NULL
 * on failure.
 */
pi_rate_controller_t* pi_rate_controller_create_default(int samplerate,
                                                        double interval,
                                                        int target_level);

/**
 * @brief Creates a PI rate controller with specified gains.
 *
 * @param samplerate The sample rate in Hz.
 * @param interval The control interval in seconds.
 * @param target_level The target buffer level in samples.
 * @param kp Proportional gain.
 * @param ki Integral gain.
 * @return A pointer to the newly created pi_rate_controller_t instance, or NULL
 * on failure.
 */
pi_rate_controller_t* pi_rate_controller_create(int samplerate, double interval,
                                                int target_level, double kp,
                                                double ki);

/**
 * @brief Updates the PI controller with a new level measurement and returns the
 * new speed multiplier.
 *
 * @param pi Pointer to the PI controller instance.
 * @param level The current measured buffer level.
 * @return The new speed multiplier.
 */
double pi_rate_controller_next(pi_rate_controller_t* pi, double level);

/**
 * @brief Frees a PI rate controller instance.
 *
 * @param pi Pointer to the PI controller to free.
 */
void pi_rate_controller_free(pi_rate_controller_t* pi);

// MARK: - Averager

/**
 * @brief Windowed arithmetic mean helper.
 *
 * The producer adds one sample per processed chunk; the rate-adjust tick reads
 * `average` once per adjust period and calls `restart()` to begin the next
 * window. The effect is a simple boxcar low-pass that filters chunk-level noise
 * out of the controller's input.
 */
typedef struct {
  double sum; /**< Sum of the samples in the current window. */
  int count;  /**< Number of samples added to the current window. */
} averager_t;

/**
 * @brief Initializes an averager.
 *
 * @param avg Pointer to the averager to initialize.
 */
void averager_init(averager_t* avg);

/**
 * @brief Adds a sample value to the averager.
 *
 * @param avg Pointer to the averager.
 * @param value The value to add.
 */
void averager_add(averager_t* avg, double value);

/**
 * @brief Restarts the averager window, resetting sum and count.
 *
 * @param avg Pointer to the averager.
 */
void averager_restart(averager_t* avg);

/**
 * @brief Calculates the mean of the samples added since the last restart.
 *
 * @param avg Pointer to the averager.
 * @param out_val Pointer to store the calculated average.
 * @return true if an average was calculated (count > 0), false otherwise.
 */
bool averager_get_average(const averager_t* avg, double* out_val);

// MARK: - Stopwatch

/**
 * @brief Monotonic elapsed-time helper.
 *
 * Backed by `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` (on Darwin) or
 * equivalent, which is a fast read (typically vDSO) suitable for invocation on
 * every processed audio chunk.
 */
typedef struct {
  uint64_t start_ns; /**< Start time in nanoseconds. */
} stopwatch_t;

/**
 * @brief Initializes a stopwatch.
 *
 * @param sw Pointer to the stopwatch to initialize.
 */
void stopwatch_init(stopwatch_t* sw);

/**
 * @brief Restarts the stopwatch, resetting the start time to now.
 *
 * @param sw Pointer to the stopwatch.
 */
void stopwatch_restart(stopwatch_t* sw);

/**
 * @brief Calculates elapsed seconds since the last initialization or restart.
 *
 * @param sw Pointer to the stopwatch.
 * @return The elapsed time in seconds.
 */
double stopwatch_elapsed_seconds(const stopwatch_t* sw);

#endif  // CLIB_ENGINE_RATE_CONTROLLER_H
