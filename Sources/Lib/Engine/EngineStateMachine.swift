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

import DSPConfig
import Synchronization

final class EngineStateMachine: @unchecked Sendable {
  private let stateRaw: Atomic<UInt8> = Atomic(ProcessingState.inactive.rawByte)
  private let stopOnce: Atomic<Bool> = Atomic(false)
  /// See class-level note for the publication discipline.
  private var _stopReason: ProcessingStopReason?

  /// Current state. Acquire-load; pairs with `setState`'s release-store.
  var state: ProcessingState {
    ProcessingState(rawByte: stateRaw.load(ordering: .acquiring))
  }

  /// Set the engine state. Release-store; pairs with the
  /// acquire-load in `state`. The release on a transition to
  /// `.inactive` is also what publishes `_stopReason` to readers.
  func setState(_ newValue: ProcessingState) {
    stateRaw.store(newValue.rawByte, ordering: .releasing)
  }

  /// Stop reason set by the most recent `beginStop` winner. Only
  /// guaranteed visible to readers that have observed
  /// `state == .inactive` via acquire-load.
  var stopReason: ProcessingStopReason? { _stopReason }

  /// CAS-guarded "first caller wins". The winner gets to set the
  /// stop reason and proceeds with teardown; subsequent concurrent
  /// callers see `false` and return without disturbing state.
  ///
  /// The reason is written before any subsequent `setState(.inactive)`
  /// release, which is what makes it safely observable by other
  /// threads that acquire-load the state.
  func beginStop(reason: ProcessingStopReason) -> Bool {
    let result = stopOnce.compareExchange(
      expected: false, desired: true,
      ordering: .acquiringAndReleasing
    )
    guard result.exchanged else { return false }
    _stopReason = reason
    return true
  }

}
