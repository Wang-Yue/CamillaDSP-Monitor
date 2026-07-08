#ifndef CLIB_ENGINE_ENGINE_STATE_MACHINE_H
#define CLIB_ENGINE_ENGINE_STATE_MACHINE_H

/**
 * @file engine_state_machine.h
 * @brief Engine state machine and stop-reason publication.
 *
 * Concurrency model
 * -----------------
 *   * `state` is an `_Atomic uint8_t` holding the raw byte encoding of
 *     `processing_state_t`. Every read uses acquire ordering; every
 *     write uses release ordering.
 *   * `stop_reason` is published using the *release-store on `state`
 *     to `PROCESSING_STATE_INACTIVE`* as the synchronisation edge. A reader
 * that acquire-loads `state` and observes `PROCESSING_STATE_INACTIVE` is
 * guaranteed by release-acquire ordering to see the writer's prior
 *     `_stopReason` assignment. Readers that have not yet observed
 *     `PROCESSING_STATE_INACTIVE` may see a stale (or `nil`) reason — that's
 * fine, the public API only treats `stop_reason` as meaningful once the engine
 * has settled.
 *   * `engine_state_machine_begin_stop` is gated by a `compareExchange` so only
 *     one caller wins the teardown — the loser sees `false` and
 *     returns. This protects against the common race where the
 *     capture thread reports a format change at the same moment the
 *     actor is asking us to stop.
 */

#include <stdbool.h>

#include "Config/engine_config_types.h"

#ifndef ENGINE_STOP_CALLBACK_T_DEFINED
#define ENGINE_STOP_CALLBACK_T_DEFINED
/**
 * @brief Callback function type for engine stop events.
 *
 * @param ctx User-defined context pointer passed to the callback.
 * @param reason The reason why the engine stopped.
 */
typedef void (*engine_stop_callback_t)(void* ctx,
                                       processing_stop_reason_t reason);
#endif

/**
 * @brief Structure representing the engine state machine.
 */
typedef struct engine_state_machine engine_state_machine_t;

/**
 * @brief Creates a new engine state machine instance.
 *
 * @return A pointer to the newly created engine_state_machine_t instance,
 *         or NULL if memory allocation failed.
 */
engine_state_machine_t* engine_state_machine_create(void);

/**
 * @brief Frees an engine state machine instance.
 *
 * @param sm Pointer to the engine state machine to free.
 */
void engine_state_machine_free(engine_state_machine_t* sm);

/**
 * @brief Gets the current state of the engine.
 *
 * Acquire-load; pairs with `engine_state_machine_set_state`'s release-store.
 *
 * @param sm Pointer to the engine state machine.
 * @return The current processing state.
 */
processing_state_t engine_state_machine_get_state(
    const engine_state_machine_t* sm);

/**
 * @brief Sets the engine state.
 *
 * Release-store; pairs with the acquire-load in
 * `engine_state_machine_get_state`. The release on a transition to inactive is
 * also what publishes `stop_reason` to readers.
 *
 * @param sm Pointer to the engine state machine.
 * @param new_state The new processing state to set.
 */
void engine_state_machine_set_state(engine_state_machine_t* sm,
                                    processing_state_t new_state);

/**
 * @brief Gets the stop reason.
 *
 * Stop reason set by the most recent `engine_state_machine_begin_stop` winner.
 * Only guaranteed visible to readers that have observed inactive state via
 * acquire-load.
 *
 * @param sm Pointer to the engine state machine.
 * @return A pointer to the stop reason, or NULL if not set.
 */
const processing_stop_reason_t* engine_state_machine_get_stop_reason(
    const engine_state_machine_t* sm);

/**
 * @brief Attempts to transition the state machine to a stopping state.
 *
 * CAS-guarded "first caller wins". The winner gets to set the stop reason
 * and proceeds with teardown; subsequent concurrent callers see `false`
 * and return without disturbing state.
 *
 * The reason is written before any subsequent state transition to inactive
 * release, which is what makes it safely observable by other threads that
 * acquire-load the state.
 *
 * @param sm Pointer to the engine state machine.
 * @param reason The reason for stopping.
 * @return true if the caller won the CAS and set the stop reason, false
 * otherwise.
 */
bool engine_state_machine_begin_stop(engine_state_machine_t* sm,
                                     processing_stop_reason_t reason);

#endif  // CLIB_ENGINE_ENGINE_STATE_MACHINE_H
