#ifndef CLIB_FILTERS_BIQUAD_H
#define CLIB_FILTERS_BIQUAD_H

/**
 * @file biquad.h
 * @brief Biquad filter coefficient computation and filtering operations.
 *
 * This file provides structures and functions for computing biquad filter
 * coefficients from high-level parameters and executing the filter on audio
 * signals.
 */

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/config_error.h"
#include "Config/filter_config_types.h"

/**
 * @struct biquad_coefficients_t
 * @brief Structure holding the transfer function coefficients of a biquad
 * filter.
 *
 * The transfer function is defined as:
 * \f$ H(z) = \frac{b_0 + b_1 z^{-1} + b_2 z^{-2}}{1 + a_1 z^{-1} + a_2 z^{-2}}
 * \f$
 */
typedef struct {
  double b0; /**< Numerator coefficient for \f$z^0\f$ */
  double b1; /**< Numerator coefficient for \f$z^{-1}\f$ */
  double b2; /**< Numerator coefficient for \f$z^{-2}\f$ */
  double a1; /**< Denominator coefficient for \f$z^{-1}\f$ */
  double a2; /**< Denominator coefficient for \f$z^{-2}\f$ */
} biquad_coefficients_t;

/**
 * @brief Returns coefficients for a passthrough filter (no effect).
 *
 * @return Biquad coefficients representing an identity filter.
 */
static inline biquad_coefficients_t biquad_coefficients_passthrough(void) {
  biquad_coefficients_t c = {1.0, 0.0, 0.0, 0.0, 0.0};
  return c;
}

/**
 * @brief Computes biquad coefficients from high-level parameters.
 *
 * @param params The high-level biquad parameters.
 * @param sample_rate The sample rate in Hz.
 * @param out_coeffs Pointer to store the computed coefficients.
 * @return `true` if computation was successful, `false` otherwise.
 */
bool biquad_coefficients_compute(const biquad_parameters_t* params,
                                 int sample_rate,
                                 biquad_coefficients_t* out_coeffs);

/**
 * @brief Computes the magnitude response in dB at a given frequency.
 *
 * Uses the analytic transfer function \f$ H(z=e^{j\omega}) \f$ — no time-domain
 * simulation needed.
 *
 * @param coeffs The biquad coefficients.
 * @param f The frequency in Hz.
 * @param sample_rate The sample rate in Hz.
 * @return The gain in dB, or 0 dB if the denominator vanishes.
 */
double biquad_coefficients_gain_db(const biquad_coefficients_t* coeffs,
                                   double f, int sample_rate);

/**
 * @brief Computes the phase response in radians at a given frequency.
 *
 * Wrapped to \f$(-\pi, \pi]\f$. Sign convention matches `atan2(Im(H), Re(H))`.
 *
 * @param coeffs The biquad coefficients.
 * @param f The frequency in Hz.
 * @param sample_rate The sample rate in Hz.
 * @return The phase in radians.
 */
double biquad_coefficients_phase_rad(const biquad_coefficients_t* coeffs,
                                     double f, int sample_rate);

/**
 * @brief Opaque structure representing a biquad filter instance (holds state).
 */
typedef struct biquad_filter biquad_filter_t;

/**
 * @brief Creates a biquad filter instance.
 *
 * @param name The name of the filter (for debugging/identification).
 * @param coeffs The initial coefficients to use.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A pointer to the created filter instance, or `NULL` on failure.
 */
biquad_filter_t* biquad_filter_create(const char* name,
                                      const biquad_coefficients_t* coeffs,
                                      config_error_t* err);

/**
 * @brief Processes an array of samples through the biquad filter.
 *
 * In-place processing.
 *
 * @param filter The filter instance.
 * @param waveform The input/output waveform buffer.
 * @param count The number of samples to process.
 */
void biquad_filter_process(biquad_filter_t* filter, mutable_waveform_t waveform,
                           size_t count);

/**
 * @brief Processes a single sample through the biquad filter.
 *
 * @param filter The filter instance.
 * @param sample The input sample.
 * @return The processed output sample.
 */
double biquad_filter_process_single(biquad_filter_t* filter, double sample);

/**
 * @brief Updates the filter parameters (coefficients) from a new configuration.
 *
 * @param filter The filter instance.
 * @param config The new filter configuration.
 * @param sample_rate The sample rate in Hz.
 */
void biquad_filter_update_parameters(biquad_filter_t* filter,
                                     const filter_config_t* config,
                                     int sample_rate);

/**
 * @brief Transfers internal history state (delay line registers) from src to
 * dest.
 *
 * @param dest The destination biquad filter instance.
 * @param src The source biquad filter instance.
 */
void biquad_filter_transfer_state(biquad_filter_t* dest,
                                  const biquad_filter_t* src);

/**
 * @brief Frees the biquad filter instance.
 *
 * @param filter The filter instance to free.
 */
void biquad_filter_free(biquad_filter_t* filter);

#endif  // CLIB_FILTERS_BIQUAD_H
