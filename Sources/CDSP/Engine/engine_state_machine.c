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

#include "engine_state_machine.h"

#include <stdatomic.h>
#include <stdint.h>

struct engine_state_machine {
  /** Raw atomic state representation. */
  _Atomic uint8_t state_raw;
  /** Atomic flag to ensure stop logic is executed only once. */
  _Atomic bool stop_once;
  /** The reason the engine stopped. See file-level note for publication
   * discipline. */
  processing_stop_reason_t stop_reason;
};

#include <stdlib.h>
#include <string.h>

engine_state_machine_t* engine_state_machine_create(void) {
  engine_state_machine_t* sm =
      (engine_state_machine_t*)malloc(sizeof(engine_state_machine_t));
  if (!sm) return NULL;
  atomic_init(&sm->state_raw,
              processing_state_to_raw_byte(PROCESSING_STATE_INACTIVE));
  atomic_init(&sm->stop_once, false);
  memset(&sm->stop_reason, 0, sizeof(processing_stop_reason_t));
  sm->stop_reason.type = STOP_REASON_NONE;
  return sm;
}

void engine_state_machine_free(engine_state_machine_t* sm) {
  if (!sm) return;
  free(sm);
}

/// Current state. Acquire-load; pairs with `setState`'s release-store.
processing_state_t engine_state_machine_get_state(
    const engine_state_machine_t* sm) {
  if (!sm) return PROCESSING_STATE_INACTIVE;
  // Use acquire memory order to ensure that any reads of other shared state
  // (like stop_reason) after this call will see the values written before
  // the corresponding release-store in set_state.
  uint8_t raw = atomic_load_explicit(&sm->state_raw, memory_order_acquire);
  return processing_state_from_raw_byte(raw);
}

/// Set the engine state. Release-store; pairs with the
/// acquire-load in `state`. The release on a transition to
/// `.inactive` is also what publishes `_stopReason` to readers.
void engine_state_machine_set_state(engine_state_machine_t* sm,
                                    processing_state_t new_state) {
  if (!sm) return;
  uint8_t raw = processing_state_to_raw_byte(new_state);
  // Use release memory order to ensure that all prior writes (specifically,
  // the stop_reason written in begin_stop) are visible to any thread that
  // reads the state with acquire ordering and observes this new state.
  atomic_store_explicit(&sm->state_raw, raw, memory_order_release);
}

/// Stop reason set by the most recent `beginStop` winner. Only
/// guaranteed visible to readers that have observed
/// `state == .inactive` via acquire-load.
const processing_stop_reason_t* engine_state_machine_get_stop_reason(
    const engine_state_machine_t* sm) {
  if (!sm) return NULL;
  return &sm->stop_reason;
}

/// CAS-guarded "first caller wins". The winner gets to set the
/// stop reason and proceeds with teardown; subsequent concurrent
/// callers see `false` and return without disturbing state.
///
/// The reason is written before any subsequent `setState(.inactive)`
/// release, which is what makes it safely observable by other
/// threads that acquire-load the state.
bool engine_state_machine_begin_stop(engine_state_machine_t* sm,
                                     processing_stop_reason_t reason) {
  if (!sm) return false;
  bool expected = false;
  // Use compare-exchange (CAS) to ensure that only the first thread to request
  // a stop succeeds. We use acq_rel ordering because we are performing a
  // read-modify-write operation that needs to synchronize with other threads.
  bool exchanged = atomic_compare_exchange_strong_explicit(
      &sm->stop_once, &expected, true, memory_order_acq_rel,
      memory_order_acquire);
  if (!exchanged) return false;

  // The winner thread writes the stop reason. This write is synchronized with
  // other threads via the state change to INACTIVE using set_state(...,
  // INACTIVE).
  sm->stop_reason = reason;
  return true;
}
