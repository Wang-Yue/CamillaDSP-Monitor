// Processing thread body. Drains the capture→processing SPSC queue,
// runs each chunk through the (optional) resampler and the pipeline,
// then enqueues the result on the processing→playback queue.
//
// State ownership
// ---------------
// The pre-allocated scratch chunks (`resamplerScratch`,
// `pipelineScratch`) are owned by this loop and only mutated here.
// The resampler's own internal state is also single-threaded: the
// playback thread publishes a relative ratio via the shared atomic,
// and the processing thread consumes it once per chunk through
// `setRelativeRatio`. No cross-thread mutation of resampler state.
//
// Audio-thread invariants
// -----------------------
//   * No allocations in the steady state. Both scratch chunks are
//     sized at init for the worst case across the configured
//     rate-adjust range.
//   * No locks. The shared SPSC queues + semaphores carry chunks
//     and wakeups; the resampler ratio is an atomic Double.
//   * The thread sets a real-time scheduling policy on entry so the
//     OS prefers it over background work.

import DSPAudio
import DSPConfig
import DSPDoP
import DSPLogging
import DSPPipeline
import DSPResampler
import Foundation
import Synchronization

/// `@unchecked Sendable` is a *transfer* vouch, not a *share*
/// vouch: the instance is safe to cross the Thread spawn boundary
/// because exactly one thread (the loop thread) ever touches it
/// after `run()` is invoked. The scratch chunks have no internal
/// synchronisation and are *not* safe to use from multiple threads
/// concurrently.
final class EngineProcessingLoop: @unchecked Sendable {
  private let logger = Logger(label: "dsp.processing")

  private let shared: EngineSharedState
  private let stateMachine: EngineStateMachine
  private let pipelineRate: Int
  private let resampler: AudioResampler?

  private let pipelineQueue = SPSCQueue<Pipeline>(minimumCapacity: 2)
  private var activePipeline: Pipeline
  private var resamplerScratch: AudioChunk
  private var pipelineScratch: AudioChunk

  private let onStop: (ProcessingStopReason) -> Void

  init(
    shared: EngineSharedState,
    stateMachine: EngineStateMachine,
    pipelineRate: Int,
    resampler: AudioResampler?,
    pipeline: Pipeline,
    resamplerScratch: AudioChunk,
    pipelineScratch: AudioChunk,
    onStop: @escaping (ProcessingStopReason) -> Void
  ) {
    self.shared = shared
    self.stateMachine = stateMachine
    self.pipelineRate = pipelineRate
    self.resampler = resampler
    self.activePipeline = pipeline
    self.resamplerScratch = resamplerScratch
    self.pipelineScratch = pipelineScratch
    self.onStop = onStop
  }

  func run() {
    logger.info("Processing thread started")
    setRealtimeThreadPriority(
      name: "Processing", bufferFrames: pipelineScratch.frames, sampleRate: pipelineRate)

    var scratchPool = RoundRobinChunkPool(
      capacity: shared.processedQueue.capacity + 4,
      frames: pipelineScratch.frames,
      channels: pipelineScratch.channels
    )

    var processedCount = 0

    while !shared.shouldStop.load(ordering: .acquiring) {
      shared.capturedSemaphore.wait()
      if shared.shouldStop.load(ordering: .acquiring) { break }

      // Drain everything the capture thread enqueued since the last
      // wake. One semaphore signal can correspond to multiple
      // enqueues if the producer outran us briefly; the inner loop
      // catches up before we wait again.
      while var chunk = shared.capturedQueue.dequeue() {
        if shared.shouldStop.load(ordering: .acquiring) { return }
        processedCount += 1

        do {
          // Resample if configured. The desired ratio is published
          // by the rate-adjust controller via `shared.resamplerRatio`;
          // we sync the resampler to it once per chunk. The
          // resampler's internal state is otherwise owned exclusively
          // by this thread, so no lock is required.
          if let resampler = resampler {
            resampler.setRelativeRatio(shared.resamplerRatio.value)

            // Write into the pre-sized output scratch (sized to
            // `resampler.maxOutputFrames`), then make that scratch
            // our working chunk. We can't `swap` here — a non-1:1
            // resampler has different input/output chunk sizes, so
            // swapping would leave scratch holding a too-small array
            // on the next iteration.
            try resampler.process(input: chunk, into: &resamplerScratch)
            chunk = resamplerScratch
          }

          // Run through the pipeline using pre-allocated output
          // scratch.
          if let nextPipeline = pipelineQueue.dequeue() {
            activePipeline = nextPipeline
          }

          if stateMachine.state == .paused {
            continue
          }

          var currentScratch = scratchPool.next()
          try activePipeline.process(input: chunk, into: &currentScratch)
          chunk = currentScratch

          if !shared.processedQueue.enqueue(chunk) {
            logger.warning(
              "Playback queue full, dropping processed chunk #%d", .int(processedCount))
          }
          shared.processedSemaphore.signal()
        } catch {
          logger.error("Processing error: %s", .string("\(error)"))
          onStop(.unknownError("\(error)"))
          return
        }
      }
    }
    logger.info("Processing thread stopped")
  }

  func setPipeline(_ newPipeline: sending Pipeline) {
    _ = pipelineQueue.enqueue(newPipeline)
  }
}
