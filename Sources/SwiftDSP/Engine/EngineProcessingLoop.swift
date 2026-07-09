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
//   * No allocations in the steady state. Output chunks are obtained
//     from a pre-allocated `RoundRobinChunkPool`, and the resampler
//     scratch chunk is pre-allocated at init.
//   * No locks. The shared SPSC queues + semaphores carry chunks
//     and wakeups; the resampler ratio is an atomic Double.
//   * The thread sets a real-time scheduling policy on entry so the
//     OS prefers it over background work.

import DSPConfig
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
  private let processingParams: ProcessingParameters
  private let pipelineRate: Int
  private let resampler: AudioResampler?
  private let dopEncoder: DoPEncoder
  private let nextPipelineSlot = AtomicReference<Pipeline>()
  private var activePipeline: Pipeline
  private var resamplerScratch: AudioChunk
  private var pipelineScratch: AudioChunk

  private let onChunkCaptured: (@Sendable (AudioChunk) -> Void)?
  private let onChunkProcessed: (@Sendable (AudioChunk) -> Void)?

  private let onStop: (ProcessingStopReason) -> Void

  init(
    shared: EngineSharedState,
    stateMachine: EngineStateMachine,
    processingParams: ProcessingParameters,
    pipelineRate: Int,
    resampler: AudioResampler?,
    pipeline: Pipeline,
    dopEncoder: DoPEncoder,
    resamplerScratch: AudioChunk,
    pipelineScratch: AudioChunk,
    onChunkCaptured: (@Sendable (AudioChunk) -> Void)?,
    onChunkProcessed: (@Sendable (AudioChunk) -> Void)?,
    onStop: @escaping (ProcessingStopReason) -> Void
  ) {
    self.shared = shared
    self.stateMachine = stateMachine
    self.processingParams = processingParams
    self.pipelineRate = pipelineRate
    self.resampler = resampler
    self.activePipeline = pipeline
    self.dopEncoder = dopEncoder
    self.resamplerScratch = resamplerScratch
    self.pipelineScratch = pipelineScratch
    self.onChunkCaptured = onChunkCaptured
    self.onChunkProcessed = onChunkProcessed
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

    while true {
      shared.capturedSemaphore.wait()

      let emergency = shared.shouldStop.load(ordering: .acquiring) &&
                      stateMachine.stopReason != .done
      let graceful = shared.captureFinished.load(ordering: .acquiring) &&
                     shared.capturedQueue.count == 0
      if emergency || graceful {
        break
      }

      // Drain everything the capture thread enqueued since the last
      // wake. One semaphore signal can correspond to multiple
      // enqueues if the producer outran us briefly; the inner loop
      // catches up before we wait again.
      while var chunk = shared.capturedQueue.dequeue() {
        processedCount += 1

        do {
          // Resample if configured. The desired ratio is published
          // by the rate-adjust controller via `shared.resamplerRatio`;
          // we sync the resampler to it once per chunk. The
          // resampler's internal state is otherwise owned exclusively
          // by this thread, so no lock is required.
          var resStart: UInt64 = 0
          var resEnd: UInt64 = 0
          if let resampler = resampler {
            resampler.setRelativeRatio(shared.resamplerRatio.value)

            resStart = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
            try resampler.process(input: chunk, into: &resamplerScratch)
            resEnd = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
            chunk = resamplerScratch
          }

          // Pre-processing tap for visualisation.
          onChunkCaptured?(chunk)

          // Run through the pipeline using pre-allocated output
          // scratch.
          if let nextPipeline = nextPipelineSlot.exchange(nil) {
            nextPipeline.transferState(from: activePipeline)
            _ = shared.pipelineGarbageQueue.enqueue(activePipeline)
            activePipeline = nextPipeline
          }

          if stateMachine.state == .paused {
            continue
          }

          var currentScratch = scratchPool.next()
          let pipeStart = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
          try activePipeline.process(input: chunk, into: &currentScratch)
          let pipeEnd = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
          chunk = currentScratch

          let frames = chunk.validFrames
          if frames > 0 {
            let chunkDurationNs = UInt64(frames) * 1_000_000_000 / UInt64(pipelineRate)
            if chunkDurationNs > 0 {
              let pipeDurationNs = pipeEnd &- pipeStart
              processingParams.processingLoad.value =
                Double(pipeDurationNs) / Double(chunkDurationNs)

              if resampler != nil {
                let resDurationNs = resEnd &- resStart
                processingParams.resamplerLoad.value =
                  Double(resDurationNs) / Double(chunkDurationNs)
              } else {
                processingParams.resamplerLoad.value = 0.0
              }
            }
          }

          var clipped: UInt64 = 0
          for ch in 0..<chunk.channels {
            let buffer = chunk[ch]
            for f in 0..<chunk.validFrames {
              if buffer[f] > 1.0 || buffer[f] < -1.0 {
                clipped += 1
              }
            }
          }
          if clipped > 0 {
            processingParams.clippedSamples.wrappingAdd(clipped, ordering: .relaxed)
          }

          _ = processingParams.updatePlaybackLevels(from: chunk)

          onChunkProcessed?(chunk)

          // Encode PCM to DoP in place if enabled
          dopEncoder.encode(chunk: &chunk)

          while !shared.processedQueue.enqueue(chunk) {
            if shared.shouldStop.load(ordering: .acquiring) { break }
            Thread.sleep(forTimeInterval: 0.002)
          }
          shared.processedSemaphore.signal()
        } catch {
          logger.error("Processing error: %s", .string("\(error)"))
          onStop(.unknownError("\(error)"))
          return
        }
      }
    }
    shared.processingFinished.store(true, ordering: .releasing)
    shared.processedSemaphore.signal()

    logger.info("Processing thread stopped")
  }

  func setPipeline(_ newPipeline: sending Pipeline) {
    nextPipelineSlot.store(newPipeline)
  }
}
