#ifndef CLIB_DOP_SIGMA_DELTA_MODULATOR_H
#define CLIB_DOP_SIGMA_DELTA_MODULATOR_H

#include "Config/engine_config_types.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Sigma-delta modulator for DSD oversampling.
///
/// Heap-backed fixed storage for the non-trellis path's two-state
/// ping-pong. Layout: `non_trellis_state[0..7]` is slot 0,
/// `non_trellis_state[8..15]` is slot 1; `idx ∈ {0,1}` selects which slot
/// is current. Replaces the `SDMState.state` `[Double]` arrays the
/// per-sample loop used to mutate via `inout`, which triggered an Array
/// CoW allocation on every sample.
///
/// `cached_a` / `cached_g` mirror the selected filter's `a` and `g`
/// coefficients into pointer storage so the hot loop avoids re-copying
/// the SDMFilter struct (and its `[Double]` ARC traffic) on every call.
typedef struct {
    int idx;
    double prev_y;
    double non_trellis_state[16];
    double cached_a[8];
    double cached_g[8];
    int cached_order;
    sdm_filter_t name;
    uint32_t freq;
} sigma_delta_modulator_t;

sigma_delta_modulator_t* sigma_delta_modulator_create(sdm_filter_t filter_name, uint32_t freq);
void sigma_delta_modulator_init(sigma_delta_modulator_t* mod, sdm_filter_t filter_name, uint32_t freq);
double sigma_delta_modulator_sample(sigma_delta_modulator_t* mod, double x);
void sigma_delta_modulator_free(sigma_delta_modulator_t* mod);

#ifdef __cplusplus
}
#endif

#endif // CLIB_DOP_SIGMA_DELTA_MODULATOR_H
