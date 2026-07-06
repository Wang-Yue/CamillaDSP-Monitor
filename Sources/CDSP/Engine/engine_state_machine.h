#ifndef CLIB_ENGINE_ENGINE_STATE_MACHINE_H
#define CLIB_ENGINE_ENGINE_STATE_MACHINE_H

// Engine state machine + stop-reason publication.
//
// Concurrency model
// -----------------
//   * `state` is an `Atomic<UInt8>` holding the raw byte encoding of
//     `ProcessingState`. Every read uses acquire ordering; every
//     write uses release ordering.
//   * `stopReason` is published using the *release-store on `state`
//     to `.inactive`* as the synchronisation edge. A reader that
//     acquire-loads `state` and observes `.inactive` is guaranteed
//     by release-acquire ordering to see the writer's prior
//     `_stopReason` assignment. Readers that have not yet observed
//     `.inactive` may see a stale (or `nil`) reason — that's fine,
//     the public API only treats `stopReason` as meaningful once
//     the engine has settled.
//   * `beginStop(reason:)` is gated by a `compareExchange` so only
//     one caller wins the teardown — the loser sees `false` and
//     returns. This protects against the common race where the
//     capture thread reports a format change at the same moment the
//     actor is asking us to stop.

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "Config/engine_config_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ENGINE_STOP_CALLBACK_T_DEFINED
#define ENGINE_STOP_CALLBACK_T_DEFINED
typedef void (*engine_stop_callback_t)(void* ctx,
                                       processing_stop_reason_t reason);
#endif

typedef struct {
  _Atomic uint8_t state_raw;
  _Atomic bool stop_once;
  /// See class-level note for the publication discipline.
  processing_stop_reason_t stop_reason;
} engine_state_machine_t;

engine_state_machine_t* engine_state_machine_create(void);
void engine_state_machine_free(engine_state_machine_t* sm);

/// Current state. Acquire-load; pairs with `setState`'s release-store.
processing_state_t engine_state_machine_get_state(
    const engine_state_machine_t* sm);

/// Set the engine state. Release-store; pairs with the
/// acquire-load in `state`. The release on a transition to
/// `.inactive` is also what publishes `_stopReason` to readers.
void engine_state_machine_set_state(engine_state_machine_t* sm,
                                    processing_state_t new_state);

/// Stop reason set by the most recent `beginStop` winner. Only
/// guaranteed visible to readers that have observed
/// `state == .inactive` via acquire-load.
const processing_stop_reason_t* engine_state_machine_get_stop_reason(
    const engine_state_machine_t* sm);

/// CAS-guarded "first caller wins". The winner gets to set the
/// stop reason and proceeds with teardown; subsequent concurrent
/// callers see `false` and return without disturbing state.
///
/// The reason is written before any subsequent `setState(.inactive)`
/// release, which is what makes it safely observable by other
/// threads that acquire-load the state.
bool engine_state_machine_begin_stop(engine_state_machine_t* sm,
                                     processing_stop_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif  // CLIB_ENGINE_ENGINE_STATE_MACHINE_H
