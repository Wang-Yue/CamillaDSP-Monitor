/**
 * @file sigma_delta_modulator.h
 * @brief Sigma-delta modulator for DSD oversampling.
 */

#ifndef CLIB_DOP_SIGMA_DELTA_MODULATOR_H
#define CLIB_DOP_SIGMA_DELTA_MODULATOR_H

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
typedef struct sigma_delta_modulator sigma_delta_modulator_t;

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
