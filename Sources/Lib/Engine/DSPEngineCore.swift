// top-level engine orchestrator.
//
// This class owns the *shape* of an engine run — config, sizing,
// device handles, the three audio threads — but contains no audio
// processing logic itself. Each thread body lives in its own file:
//
//   * `EngineCaptureLoop`     — capture → DoP-decode → level meter
//                               → SPSC queue.
//   * `EngineProcessingLoop`  — SPSC dequeue → resample → pipeline
//                               → SPSC enqueue.
//   * `EnginePlaybackLoop`    — SPSC dequeue → rate-adjust controller
//                               → device write.
//
// All cross-thread state (the stop flag, the SPSC queues, the
// resampler-ratio atomic) lives in `EngineSharedState`. State
// machine + stop-reason publication lives in `EngineStateMachine`.
//
// Lock-free / allocation-free guarantees
// --------------------------------------
//   * The audio threads use lock-free SPSC queues and atomics;
//     only `DispatchSemaphore` is used for signal/wait, which is
//     a kernel signaling primitive (not a lock).
//   * Every per-chunk allocation is pre-allocated at `start()` —
//     the working capture chunk, the resampler output scratch,
//     and the pipeline output scratch.
//   * The stall watchdog uses `clock_gettime_nsec_np` (vDSO read,
//     no syscall) — no `Date()` on the hot path.

import DSPAudio
import DSPBackend
import DSPConfig
import DSPLogging
import DSPPipeline
import DSPResampler
import Foundation
import Synchronization

internal final class DSPEngineCore {
  private let logger = Logger(label: "dsp.engine.core")

  // MARK: - Configuration

  internal private(set) var currentConfig: DSPConfiguration
  internal let processingParams: ProcessingParameters

  // MARK: - Shared state

  private let stateMachine = EngineStateMachine()
  private let shared = EngineSharedState()

  // MARK: - Public state surface

  internal var state: ProcessingState { stateMachine.state }
  internal var stopReason: ProcessingStopReason? { stateMachine.stopReason }

  // MARK: - Components built per run

  private var capture: CaptureBackend?
  private var playback: PlaybackBackend?
  private var processingLoop: EngineProcessingLoop?

  /// Playback-side chunk size — `resampler.maxOutputFrames` when a
  /// resampler is in use, otherwise `effectiveChunkSize`.
  private var effectivePlaybackChunkSize: Int = 0

  // MARK: - Threading

  private let threadsExitGroup = DispatchGroup()

  // MARK: - Init

  internal init(config: DSPConfiguration) {
    self.currentConfig = config
    self.processingParams = ProcessingParameters()
  }

  // MARK: - Lifecycle

  internal func start() throws {
    guard state == .inactive else {
      logger.warning("Engine already running")
      return
    }

    stateMachine.setState(.starting)
    shared.shouldStop.store(false, ordering: .releasing)
    logger.info("Starting DSP engine")

    let runtime = try buildRuntime()
    self.capture = runtime.capture
    self.playback = runtime.playback
    self.effectivePlaybackChunkSize = runtime.playbackChunkSize

    try runtime.capture.open()
    try runtime.playback.open()

    // Pre-fill the playback ring with zeros so the CoreAudio render
    // thread has a buffer of silence to drain during startup. If
    // rate adjust is enabled we match its target level; otherwise
    // we pre-fill a safe 4-chunk headroom.
    let prefillFrames: Int
    if currentConfig.devices.enableRateAdjust == true {
      prefillFrames = currentConfig.devices.targetLevel ?? (runtime.playbackChunkSize * 2)
    } else {
      prefillFrames = runtime.playbackChunkSize * 4
    }
    try runtime.playback.prefillSilence(frames: prefillFrames)

    spawnThreads(runtime: runtime)

    stateMachine.setState(.running)
    logger.info(
      "DSP engine started: %dHz, chunk=%d", .int(currentConfig.devices.samplerate),
      .int(runtime.captureChunkSize))
  }

  internal func stop(reason: ProcessingStopReason = .none) {
    // Idempotent. Only the first caller drives teardown — concurrent
    // requests (typically the captureLoop's format-change report
    // racing with the actor's `previous.stop(.none)`) just return.
    guard stateMachine.beginStop(reason: reason) else { return }
    guard state != .inactive else { return }

    logger.info("Stopping engine: %s", .string("\(reason)"))
    shared.shouldStop.store(true, ordering: .releasing)

    // Wake the loops out of their semaphore waits so they can
    // observe `shouldStop` and exit cleanly.
    shared.capturedSemaphore.signal()
    shared.processedSemaphore.signal()

    _ = threadsExitGroup.wait(timeout: .now() + 0.5)

    // Drain any chunks left in the lock-free queues before the
    // device handles go away. Prevents stale-chunk pollution if
    // the engine is restarted with a different config.
    shared.capturedQueue.drain()
    shared.processedQueue.drain()

    capture?.close()
    playback?.close()
    processingLoop = nil

    stateMachine.setState(.inactive)
    logger.info("Engine stopped")
  }

  /// Rebuild the processing pipeline against `newConfig` without
  /// touching the audio devices. The caller is responsible for
  /// verifying that `newConfig.devices == currentConfig.devices` —
  /// the `DSPEngine` actor does this comparison and falls back to a
  /// full teardown when they differ.
  internal func reloadConfig(_ newConfig: DSPConfiguration) throws {
    currentConfig = newConfig

    guard state != .inactive else { return }
    let newPipeline = try Pipeline(
      config: currentConfig, processingParams: processingParams,
      explicitChunkSize: effectivePlaybackChunkSize)
    processingLoop?.setPipeline(newPipeline)
    logger.info("Pipeline rebuilt without audio-device restart")
  }

  // MARK: - Private: runtime construction

  /// Bag of components built in `start()` and handed to each loop.
  /// Bundling them avoids passing eight parameters around and keeps
  /// the loop initialisers concise.
  private struct Runtime {
    let capture: CaptureBackend
    let playback: PlaybackBackend
    let resampler: AudioResampler?
    let pipeline: Pipeline
    let resamplerScratch: AudioChunk
    let pipelineScratch: AudioChunk
    let captureChunkSize: Int
    let playbackChunkSize: Int
    let pipelineRate: Int
    let captureRate: Int
  }

  private func buildRuntime() throws -> Runtime {
    // Resolve capture/playback rates. `capture_samplerate` is the
    // upstream knob for "capture device runs at a different rate
    // than the engine pipeline" — when set it forces the capture
    // backend to open at that rate and configures the resampler
    // with a non-1:1 base ratio. When unset both rates collapse
    // to `samplerate` and any resampler runs at 1:1 (used solely
    // as a drift-correction surface for rate-adjust).
    let pipelineRate = currentConfig.devices.samplerate
    let captureRate = currentConfig.devices.captureSamplerate ?? pipelineRate

    // Create the resampler first so we can adopt its (possibly
    // rounded) chunk size before opening the audio devices.
    let resampler: AudioResampler?
    if let resamplerConfig = currentConfig.devices.resampler {
      resampler = try createResampler(
        config: resamplerConfig,
        inputRate: captureRate,
        outputRate: pipelineRate,
        channels: currentConfig.devices.capture.channels,
        chunkSize: currentConfig.devices.chunksize)
    } else {
      resampler = nil
    }

    // Adopt the resampler's input chunk size. `SynchronousResampler`
    // rounds the requested size up to the smallest valid multiple
    // of `inputRate / gcd(in, out)`; the rest of the engine has to
    // honour that rounded value or `process(input:into:)` will
    // throw `inputSizeMismatch`. The async resamplers don't round,
    // so this is a no-op for them.
    let requestedChunkSize = currentConfig.devices.chunksize
    let captureChunkSize = resampler?.chunkSize ?? requestedChunkSize
    let playbackChunkSize = resampler?.maxOutputFrames ?? captureChunkSize
    if captureChunkSize != requestedChunkSize {
      logger.info(
        "Adopting resampler chunkSize=%d (config requested %d)",
        .int(captureChunkSize), .int(requestedChunkSize))
    }

    let capture = try createCaptureBackend(
      config: currentConfig.devices.capture,
      sampleRate: captureRate, chunkSize: captureChunkSize)
    let playback = try createPlaybackBackend(
      config: currentConfig.devices.playback,
      sampleRate: pipelineRate, chunkSize: playbackChunkSize)

    // Pre-allocate scratch buffers sized for the worst case across
    // the configured rate-adjust range.
    var resamplerScratch = AudioChunk(
      frames: resampler?.maxOutputFrames ?? captureChunkSize,
      channels: currentConfig.devices.capture.channels)
    resamplerScratch.validFrames = 0

    let pipeline = try Pipeline(
      config: currentConfig, processingParams: processingParams,
      explicitChunkSize: playbackChunkSize)

    var pipelineScratch = AudioChunk(
      frames: playbackChunkSize, channels: currentConfig.devices.playback.channels)
    pipelineScratch.validFrames = 0

    return Runtime(
      capture: capture, playback: playback, resampler: resampler, pipeline: pipeline,
      resamplerScratch: resamplerScratch, pipelineScratch: pipelineScratch,
      captureChunkSize: captureChunkSize, playbackChunkSize: playbackChunkSize,
      pipelineRate: pipelineRate, captureRate: captureRate
    )
  }

  // MARK: - Private: thread spawn

  private func spawnThreads(runtime: Runtime) {
    let captureLoop = EngineCaptureLoop(
      shared: shared,
      stateMachine: stateMachine,
      capture: runtime.capture,
      playback: runtime.playback,
      processingParams: processingParams,
      chunkSize: runtime.captureChunkSize,
      channels: currentConfig.devices.capture.channels,
      samplerate: runtime.captureRate,
      silenceThresholdDb: currentConfig.devices.silenceThreshold ?? 0,
      silenceTimeoutSeconds: currentConfig.devices.silenceTimeout ?? 0,
      onStop: { [weak self] reason in self?.stop(reason: reason) }
    )

    let processingLoop = EngineProcessingLoop(
      shared: shared,
      stateMachine: stateMachine,
      pipelineRate: runtime.pipelineRate,
      resampler: runtime.resampler,
      pipeline: runtime.pipeline,
      resamplerScratch: runtime.resamplerScratch,
      pipelineScratch: runtime.pipelineScratch,
      onStop: { [weak self] reason in self?.stop(reason: reason) }
    )

    let rateAdjustEnabled = currentConfig.devices.enableRateAdjust == true
    let adjustPeriod = currentConfig.devices.adjustPeriod ?? 10.0
    let targetLevel = currentConfig.devices.targetLevel ?? (runtime.playbackChunkSize * 2)
    let playbackLoop = EnginePlaybackLoop(
      shared: shared,
      capture: runtime.capture,
      playback: runtime.playback,
      pipelineRate: runtime.pipelineRate,
      chunkSize: runtime.playbackChunkSize,
      rateAdjustEnabled: rateAdjustEnabled,
      adjustPeriod: adjustPeriod,
      targetLevel: targetLevel,
      onStop: { [weak self] reason in self?.stop(reason: reason) }
    )

    self.processingLoop = processingLoop

    _ = spawnRealtimeThread(
      name: "dsp.capture", body: { captureLoop.run() })
    _ = spawnRealtimeThread(
      name: "dsp.processing", body: { processingLoop.run() })
    _ = spawnRealtimeThread(
      name: "dsp.playback", body: { playbackLoop.run() })
  }

  /// Wrap `Thread` construction so each spawn shares the same QoS,
  /// name pattern, and exit-group bookkeeping.
  private func spawnRealtimeThread(
    name: String, body: @escaping @Sendable () -> Void
  ) -> Thread {
    threadsExitGroup.enter()
    let group = threadsExitGroup
    let thread = Thread {
      defer { group.leave() }
      body()
    }
    thread.qualityOfService = .userInteractive
    thread.name = name
    thread.start()
    return thread
  }
}
