/**
 * @file sigma_delta_modulator.h
 * @brief Sigma-delta modulator for DSD oversampling.
 */

#ifndef CLIB_DOP_SIGMA_DELTA_MODULATOR_H
#define CLIB_DOP_SIGMA_DELTA_MODULATOR_H

#include <stdbool.h>
#include <stdint.h>

#include "Config/engine_config_types.h"

/**
 * @brief Sigma-delta modulator state.
 *
 * Heap-backed fixed storage for the modulator state.
 * `non_trellis_state[0..7]` is slot 0, `non_trellis_state[8..15]` is slot 1;
 * `idx` selects which slot is current.
 *
 * `cached_a` and `cached_g` mirror the selected filter's coefficients
 * to avoid re-copying the filter structure in the hot loop.
 */
typedef struct {
  /** Index of the current state slot (0 or 1). */
  int idx;
  /** Previous output value. */
  double prev_y;
  /** State storage for the filter (two slots of 8 doubles each). */
  double non_trellis_state[16];
  /** Cached 'a' coefficients of the filter. */
  double cached_a[8];
  /** Cached 'g' coefficients of the filter. */
  double cached_g[8];
  /** Cached filter order. */
  int cached_order;
  /** Name of the filter. */
  sdm_filter_t name;
  /** Sampling frequency. */
  uint32_t freq;
} sigma_delta_modulator_t;

/**
 * @brief Create a sigma-delta modulator.
 *
 * @param filter_name Name of the filter to use.
 * @param freq Sampling frequency.
 * @return Pointer to the created modulator instance, or NULL on failure.
 */
sigma_delta_modulator_t* sigma_delta_modulator_create(sdm_filter_t filter_name,
                                                      uint32_t freq);

/**
 * @brief Initialize a sigma-delta modulator instance.
 *
 * @param mod Pointer to the modulator instance to initialize.
 * @param filter_name Name of the filter to use.
 * @param freq Sampling frequency.
 */
void sigma_delta_modulator_init(sigma_delta_modulator_t* mod,
                                sdm_filter_t filter_name, uint32_t freq);

/**
 * @brief Process a single sample through the modulator.
 *
 * @param mod Pointer to the modulator instance.
 * @param x Input sample.
 * @return Modulated output sample.
 */
double sigma_delta_modulator_sample(sigma_delta_modulator_t* mod, double x);

/**
 * @brief Free the sigma-delta modulator.
 *
 * @param mod Pointer to the modulator instance to free.
 */
void sigma_delta_modulator_free(sigma_delta_modulator_t* mod);

#endif  // CLIB_DOP_SIGMA_DELTA_MODULATOR_H
