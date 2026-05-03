// CamillaDSP-Swift: Main DSP engine - coordinates capture, processing, and playback threads

import Foundation
import Logging
import Synchronization

/// The CamillaDSP engine implementation that owns the capture/processing/playback
/// threads. The public-facing API for CamillaDSPMonitor lives on the `DSPEngine`
/// actor (see `Engine/DSPEngine.swift`); this class is the workhorse it drives.
///
/// Marked `@unchecked Sendable` so Thread closures can capture it under Swift 6's
/// strict-concurrency checker. The internal mutable state is guarded by
/// `chunkLock`, `resamplerRatioLock`, and the actor that owns this instance.
public final class DSPEngineCore: @unchecked Sendable {
  private let logger = Logger(label: "camilladsp.engine")

  // Configuration
  public private(set) var currentConfig: CamillaDSPConfig
  public let processingParams: ProcessingParameters

  // State. Stored as Atomic<UInt8> so the audio thread can flip it
  // (capture loop sets `.paused` on silence detection and `.stalled`
  // on the read watchdog) without racing the actor's reads.
  private let _stateRaw: Atomic<UInt8> = Atomic(ProcessingState.inactive.rawByte)
  public var state: ProcessingState {
    ProcessingState(rawByte: _stateRaw.load(ordering: .acquiring))
  }
  private func setState(_ newValue: ProcessingState) {
    _stateRaw.store(newValue.rawByte, ordering: .releasing)
  }

  public private(set) var stopReason: ProcessingStopReason?
  private var shouldStop = false
  /// Set to `true` the first time `stop()` is entered. Guarantees
  /// the close-and-teardown sequence runs once even when the
  /// captureLoop and the public actor try to stop us concurrently
  /// — typical during a HAL-driven format change, where the loop
  /// stops with `.captureFormatChange` while the host receives the
  /// status update and calls `start(...)`, which in turn calls
  /// `previous.stop(.none)`. Without this guard both callers race
  /// `capture?.close()` / `playback?.close()`, causing double-
  /// dispose of the AudioUnit and a stale hog-mode release.
  private let _stopInitiated = Atomic<Bool>(false)

  // Components
  private var capture: CaptureBackend?
  private var playback: PlaybackBackend?
  private var pipeline: Pipeline?
  private var resampler: AudioResampler?

  /// The chunk size actually used by the engine for capture-side
  /// buffering. `SynchronousResampler` rounds the requested chunk
  /// size up to the smallest valid multiple of `inputRate / gcd(in,
  /// out)`, so when one is configured the engine adopts that rounded
  /// value here and uses it everywhere instead of the raw config
  /// value. Falls back to `currentConfig.devices.chunksize` when no
  /// resampler is in use.
  private var effectiveChunkSize: Int = 0

  /// The chunk size used by the playback-side ring buffer and
  /// rate-adjust queue-depth estimate. Equals `resampler.maxOutputFrames`
  /// when a resampler is in use, otherwise `effectiveChunkSize`.
  private var effectivePlaybackChunkSize: Int = 0

  /// Capture-device sample rate, resolved at `start()`. Equal to
  /// `currentConfig.devices.captureSamplerate` when set, otherwise
  /// `currentConfig.devices.samplerate`. Cached so background threads
  /// don't have to re-resolve it on every iteration.
  private var captureRate: Int = 0

  /// Pre-allocated scratch chunk that the resampler writes into. We swap it
  /// with the in-flight chunk so resampling consumes no allocations on the
  /// hot path. `nil` when no resampler is configured.
  private var resamplerScratch: AudioChunk

  // Threading
  private var captureThread: Thread?
  private var processingThread: Thread?
  private var playbackThread: Thread?
  /// Lock-free SPSC FIFO from the capture thread to the processing thread.
  /// Bounded — `enqueue` returns `false` when full so the producer can
  /// drop a chunk instead of allocating.
  private let capturedChunks = SPSCQueue<AudioChunk>(minimumCapacity: 16)
  /// Lock-free SPSC FIFO from the processing thread to the playback thread.
  private let processedChunks = SPSCQueue<AudioChunk>(minimumCapacity: 16)
  /// Wake-up signals — semaphores are signal/wait, not locks. The
  /// producer signals after `enqueue`; the consumer waits, then drains
  /// whatever was enqueued.
  private let chunkSemaphore = DispatchSemaphore(value: 0)
  private let playbackSemaphore = DispatchSemaphore(value: 0)
  private let threadsExitGroup = DispatchGroup()

  // Rate adjustment.
  //
  // The control loop lives in `playbackLoop`: every dequeued chunk
  // contributes its measured (ring-fill + queued-chunks) to an
  // `Averager`, and once per `adjust_period` seconds the average
  // is fed to a `PIRateController`. The output speed multiplier is
  // applied either via capture-clock pitch tuning (BlackHole 0.5.0+)
  // or by writing `desiredResamplerRatio`, which the processing
  // thread picks up via the resampler's `setRelativeRatio` method.
  /// Desired resampler relative ratio (~1.0). Read by the
  /// processing thread, written by the playback thread when
  /// rate-adjust is enabled and the capture device doesn't support
  /// pitch tuning. Lock-free — the writer's release-store and the
  /// reader's acquire-load establish happens-before.
  private let desiredResamplerRatio = AtomicDouble(1.0)

  /// Number of chunks the capture thread has dropped because the
  /// processing-side queue was full. Bumped from the audio thread
  /// without locking or string formatting. Exposed as a monotonic
  /// counter so a non-RT observer (the actor, the rate-adjust
  /// timer, or external monitoring) can poll it.
  private let _capturedDropCounter = Atomic<UInt64>(0)
  /// Total number of chunks dropped at the capture→processing
  /// boundary since this engine instance was created. Safe to read
  /// from any thread.
  public var capturedDropCount: UInt64 {
    _capturedDropCounter.load(ordering: .relaxed)
  }

  /// Optional callback invoked before pipeline processing, on the processing thread.
  /// Receives the raw captured audio (post-resample, pre-pipeline).
  public var onChunkCaptured: ((_ chunk: AudioChunk) -> Void)?

  /// Optional callback invoked after pipeline processing, on the processing thread.
  /// Receives the processed audio (post-pipeline, pre-playback).
  public var onChunkProcessed: ((_ chunk: AudioChunk) -> Void)?

  public init(config: CamillaDSPConfig) {
    self.currentConfig = config
    self.processingParams = ProcessingParameters(
      captureChannels: config.devices.capture.channels,
      playbackChannels: config.devices.playback.channels
    )
    // Initialize with a dummy chunk (re-allocated with correct size in start() if needed)
    self.resamplerScratch = AudioChunk(frames: 1, channels: 1)
  }

  // MARK: - Lifecycle

  public func start() throws {
    guard state == .inactive else {
      logger.warning("Engine already running")
      return
    }

    setState(.starting)
    shouldStop = false
    logger.info("Starting CamillaDSP engine")

    // Resolve capture/playback rates. `capture_samplerate` is the
    // upstream knob for "capture device runs at a different rate
    // than the engine pipeline" — when set it forces the capture
    // backend to open at that rate and configures the resampler
    // with a non-1:1 base ratio. When unset both rates collapse
    // to `samplerate` and any resampler runs at 1:1 (used solely
    // as a drift-correction surface for rate-adjust).
    let pipelineRate = currentConfig.devices.samplerate
    let captureRate = currentConfig.devices.captureSamplerate ?? pipelineRate
    self.captureRate = captureRate

    // Create resampler first so we can adopt its (possibly rounded)
    // chunk size before opening the audio devices. Input is the
    // capture device's rate, output is the engine pipeline rate.
    // With identical rates the resampler runs at 1:1 and only carries
    // rate-adjust corrections via `setRelativeRatio`.
    if let resamplerConfig = currentConfig.devices.resampler {
      resampler = createResampler(
        config: resamplerConfig,
        inputRate: captureRate,
        outputRate: pipelineRate,
        channels: currentConfig.devices.capture.channels)
    }

    // Adopt the resampler's input chunk size. `SynchronousResampler`
    // rounds the requested size up to the smallest valid multiple of
    // `inputRate / gcd(in, out)`; the rest of the engine has to honour
    // that rounded value or `process(input:into:)` will throw
    // `inputSizeMismatch`. The async resamplers don't round, so this
    // is a no-op for them.
    let requestedChunkSize = currentConfig.devices.chunksize
    let effectiveChunkSize = resampler?.chunkSize ?? requestedChunkSize
    self.effectiveChunkSize = effectiveChunkSize
    self.effectivePlaybackChunkSize = resampler?.maxOutputFrames ?? effectiveChunkSize
    if effectiveChunkSize != requestedChunkSize {
      logger.info(
        "Adopting resampler chunkSize=\(effectiveChunkSize) (config requested \(requestedChunkSize))"
      )
    }

    // Create backends sized to the adopted chunk size.
    capture = try createCaptureBackend(
      config: currentConfig, captureRate: captureRate, chunkSize: effectiveChunkSize)
    playback = try createPlaybackBackend(
      config: currentConfig, chunkSize: effectivePlaybackChunkSize)

    // Pre-allocate the resampler output scratch sized for the worst-case
    // output across the configured rate-adjust range. The resampler computes
    // `maxOutputFrames` once at init from `chunkSize` × `maxRelativeRatio`,
    // so this is a single constant — no resize logic needed at runtime.
    if let r = resampler {
      let capacity = r.maxOutputFrames
      let channels = currentConfig.devices.capture.channels
      var waveforms: [[PrcFmt]] = []
      waveforms.reserveCapacity(channels)
      for _ in 0..<channels {
        waveforms.append([PrcFmt](repeating: 0, count: capacity))
      }
      resamplerScratch = AudioChunk(waveforms: waveforms, validFrames: 0)
    }

    // Create pipeline
    pipeline = try Pipeline(
      config: currentConfig, processingParams: processingParams,
      explicitChunkSize: effectivePlaybackChunkSize)

    // Open devices
    try capture?.open()
    try playback?.open()

    // Pre-fill the playback ring with `target_level` zeros so the
    // rate-adjust controller starts in regulation rather than
    // having to chase an empty buffer. Skipped when rate-adjust
    // is disabled — the buffer fills naturally from the first
    // captured chunks.
    if currentConfig.devices.enableRateAdjust == true {
      let targetLevel = currentConfig.devices.targetLevel ?? 1024
      try playback?.prefillSilence(frames: targetLevel)
    }

    // Start threads
    threadsExitGroup.enter()
    captureThread = Thread { [weak self] in
      defer { self?.threadsExitGroup.leave() }
      self?.captureLoop()
    }
    captureThread?.qualityOfService = .userInteractive
    captureThread?.name = "camilladsp.capture"
    captureThread?.start()

    threadsExitGroup.enter()
    processingThread = Thread { [weak self] in
      defer { self?.threadsExitGroup.leave() }
      self?.processingLoop()
    }
    processingThread?.qualityOfService = .userInteractive
    processingThread?.name = "camilladsp.processing"
    processingThread?.start()

    threadsExitGroup.enter()
    playbackThread = Thread { [weak self] in
      defer { self?.threadsExitGroup.leave() }
      self?.playbackLoop()
    }
    playbackThread?.qualityOfService = .userInteractive
    playbackThread?.name = "camilladsp.playback"
    playbackThread?.start()

    // Rate adjustment runs inline in `playbackLoop` (no timer).
    // Log the gate result either way so a user who toggles
    // `enable_rate_adjust` in Monitor can confirm it reached the
    // engine config.
    if currentConfig.devices.enableRateAdjust == true {
      let period = currentConfig.devices.adjustPeriod ?? 10.0
      let targetLevel = currentConfig.devices.targetLevel ?? 1024
      let mode =
        (capture?.pitchControlSupported ?? false)
        ? "capture clock pitch"
        : "resampler ratio"
      logger.info(
        "Rate adjustment enabled (period=\(period)s, target_level=\(targetLevel), method=\(mode))")
    } else {
      logger.info("Rate adjustment disabled (enable_rate_adjust not set in config)")
    }

    setState(.running)
    logger.info(
      "CamillaDSP engine started: \(currentConfig.devices.samplerate)Hz, chunk=\(effectiveChunkSize)"
    )
  }

  public func stop(reason: ProcessingStopReason = .none) {
    // Idempotent: only the first caller runs the cleanup. A second
    // concurrent caller (typically the actor's `start()` reaching
    // `previous.stop(.none)` while the captureLoop's
    // `.captureFormatChange` stop is still in flight) just returns.
    let result = _stopInitiated.compareExchange(
      expected: false, desired: true,
      ordering: .acquiringAndReleasing
    )
    guard result.exchanged else { return }

    guard state != .inactive else { return }

    logger.info("Stopping engine: \(reason)")
    shouldStop = true
    stopReason = reason

    // Signal semaphores to unblock threads
    chunkSemaphore.signal()
    playbackSemaphore.signal()

    // Wait for threads to finish
    _ = threadsExitGroup.wait(timeout: .now() + 0.5)

    // Drain leftover stale chunks to prevent new pipeline pollution
    capturedChunks.drain()
    processedChunks.drain()

    capture?.close()
    playback?.close()

    setState(.inactive)
    logger.info("Engine stopped")
  }

  /// Rebuild the processing pipeline against `newConfig` without
  /// touching the audio devices. Caller is responsible for verifying
  /// that `newConfig.devices == currentConfig.devices` — the
  /// `DSPEngine` actor does this comparison and falls back to a full
  /// teardown when they differ.
  public func reloadConfig(_ newConfig: CamillaDSPConfig) throws {
    currentConfig = newConfig
    guard state == .running else { return }
    pipeline = try Pipeline(
      config: currentConfig, processingParams: processingParams,
      explicitChunkSize: effectivePlaybackChunkSize)
    logger.info("Pipeline rebuilt without audio-device restart")
  }

  // MARK: - Thread Loops

  private func captureLoop() {
    logger.info("Capture thread started")
    let chunkSize = effectiveChunkSize
    let samplerate = currentConfig.devices.samplerate
    let channels = currentConfig.devices.capture.channels
    var chunkCount = 0

    var chunk = AudioChunk(frames: chunkSize, channels: channels)

    // Silence-detection counter — drives the engine into `.paused`
    // when the capture signal stays below the configured threshold
    // for the configured timeout.
    var silenceCounter = SilenceCounter(
      thresholdDb: currentConfig.devices.silenceThreshold ?? 0,
      timeoutSeconds: currentConfig.devices.silenceTimeout ?? 0,
      samplerate: samplerate,
      chunksize: chunkSize
    )

    // Stall watchdog — flips to `.stalled` if capture.read returns
    // nil for more than `stallTimeout` seconds in a row. The window
    // is generous compared to a HAL buffer (typical 10–20 ms) so we
    // don't false-trip during normal startup ring-fill.
    let stallTimeout: TimeInterval = 0.5
    var lastSuccessfulRead = Date()
    var watchdogTriggered = false

    while !shouldStop {
      // Surface a HAL-level sample-rate change before doing any
      // more work. A user (or another app) flipping the device
      // rate in Audio MIDI Setup invalidates the AudioUnit's
      // configured format; the cleanest recovery is to stop and
      // let the host rebuild — same shape as upstream's
      // `StatusMessage::CaptureFormatChange` flow.
      if let rate = capture?.pendingRateChange {
        logger.info("Capture device rate changed to \(rate) Hz; stopping engine")
        stop(reason: .captureFormatChange(Int(rate.rounded())))
        return
      }
      if let rate = playback?.pendingRateChange {
        logger.info("Playback device rate changed to \(rate) Hz; stopping engine")
        stop(reason: .playbackFormatChange(Int(rate.rounded())))
        return
      }

      do {
        guard let capture = self.capture, try capture.read(frames: chunkSize, into: &chunk) else {
          if !shouldStop {
            if !watchdogTriggered,
              Date().timeIntervalSince(lastSuccessfulRead) > stallTimeout
            {
              setState(.stalled)
              watchdogTriggered = true
              logger.warning("Capture device stalled — no data for \(stallTimeout)s")
            }
            Thread.sleep(forTimeInterval: 0.001)
          }
          continue
        }
        lastSuccessfulRead = Date()
        if watchdogTriggered {
          watchdogTriggered = false
          logger.info("Capture recovered from stall")
        }

        chunkCount += 1

        let loudestPeak = processingParams.updateCaptureLevels(from: chunk)

        // Update silence detector with the loudest channel's
        // peak. Returns the desired engine state — `.paused`
        // once we cross the timeout, `.running` otherwise. We
        // only flip when the value actually changes to avoid
        // hammering the atomic from the audio thread.
        let desired = silenceCounter.update(signalPeakDb: loudestPeak)
        let current = state
        if desired != current && current != .stalled {
          setState(desired)
        }

        // Enqueue for processing. The lock-free SPSC queue is
        // bounded; on overflow we drop the chunk rather than
        // allocate. We bump an atomic counter instead of
        // calling the logger — `swift-log` can take internal
        // locks and format strings, both of which are a poor
        // fit for the audio-priority capture thread (and
        // particularly bad precisely when the system is
        // already overloaded). The processing loop reports
        // the accumulated drop count once per ~200 chunks.
        if !capturedChunks.enqueue(chunk) {
          _capturedDropCounter.wrappingAdd(1, ordering: .relaxed)
        }
        chunkSemaphore.signal()

      } catch {
        logger.error("Capture error: \(error)")
        stop(reason: .captureError("\(error)"))
        return
      }
    }
    logger.info("Capture thread stopped")
  }

  private func processingLoop() {
    logger.info("Processing thread started")

    // Set real-time thread priority
    setRealtimePriority()

    var processedCount = 0

    while !shouldStop {
      chunkSemaphore.wait()
      if shouldStop { break }

      // Drain everything the capture thread enqueued since the
      // last wake. One semaphore.signal can correspond to multiple
      // enqueues if the producer outran us briefly; the inner loop
      // catches up before we wait again.
      while var chunk = capturedChunks.dequeue() {
        if shouldStop { return }
        processedCount += 1

        do {
          let startTime = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)

          // Resample if needed. The desired ratio is published
          // by the rate-adjust timer through `desiredResamplerRatio`;
          // we sync the resampler to it once per chunk (the
          // resampler's internal state is otherwise owned
          // exclusively by this thread, so no lock is required).
          if let resampler = resampler {
            // `desiredResamplerRatio` carries the rate-adjust
            // *relative* multiplier (≈1.0). The resampler
            // combines it with the base output/input ratio
            // internally — pushing it via `setRelativeRatio`
            // every chunk is cheap (just a Double store).
            resampler.setRelativeRatio(desiredResamplerRatio.value)

            // Write into the pre-sized output scratch (sized to
            // `resampler.maxOutputFrames`), then make that scratch our
            // working chunk. The two share array storage via ARC; once
            // playback drains its queue slot the storage refcount drops
            // to 1 and the next `resampler.process` write is in place.
            // We can't `swap` here — a non-1:1 resampler has different
            // input/output chunk sizes, so swap would leave scratch
            // holding a too-small array on the next iteration.
            try resampler.process(input: chunk, into: &resamplerScratch)
            chunk = resamplerScratch
          }

          // Pre-processing tap for visualization.
          onChunkCaptured?(chunk)

          // Process through pipeline.
          try pipeline?.process(chunk: &chunk)

          // Measure processing load (covers resample + pipeline).
          // Wall-clock duration of one input chunk is `effectiveChunkSize
          // / captureRate`; the processing thread must clear that budget
          // per chunk to keep up with capture.
          let elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - startTime
          let chunkDuration =
            Double(effectiveChunkSize) / Double(captureRate) * 1_000_000_000.0
          processingParams.processingLoad = Double(elapsed) / chunkDuration * 100.0

          _ = processingParams.updatePlaybackLevels(from: chunk)

          onChunkProcessed?(chunk)

          if !processedChunks.enqueue(chunk) {
            logger.warning("Playback queue full, dropping processed chunk #\(processedCount)")
          }
          playbackSemaphore.signal()

        } catch {
          logger.error("Processing error: \(error)")
          stop(reason: .unknownError("\(error)"))
          return
        }
      }
    }
    logger.info("Processing thread stopped")
  }

  private func playbackLoop() {
    logger.info("Playback thread started")

    // Rate-adjust state lives entirely on this thread — controller,
    // averager, and stopwatch are mutated only here, so no
    // synchronisation is needed. The output speed is published to
    // the processing thread via `desiredResamplerRatio` (atomic)
    // or applied directly to the capture clock when supported.
    let rateAdjustEnabled = currentConfig.devices.enableRateAdjust == true
    let adjustPeriod = currentConfig.devices.adjustPeriod ?? 10.0
    let targetLevel = currentConfig.devices.targetLevel ?? 1024
    let pipelineRate = currentConfig.devices.samplerate
    // Chunks queued in `processedChunks` carry post-resample audio at
    // `pipelineRate`, so the queue-depth estimate uses the resampler's
    // output chunk size, not the capture-side input size.
    let chunkSize = effectivePlaybackChunkSize
    let pitchSupported = capture?.pitchControlSupported ?? false

    var rateController: PIRateController? = nil
    var averager = Averager()
    var stopwatch = Stopwatch()
    var lastSpeed: Double = 1.0
    if rateAdjustEnabled {
      rateController = PIRateController(
        samplerate: pipelineRate,
        interval: adjustPeriod,
        targetLevel: targetLevel
      )
      stopwatch.restart()
    }

    while !shouldStop {
      playbackSemaphore.wait()
      if shouldStop { break }

      while let chunk = processedChunks.dequeue() {
        if shouldStop { return }

        // Sample the buffer fill *before* writing — measures
        // what the rate-adjust controller cares about: how
        // much already-produced audio is queued in front of
        // the device. Matches upstream's measurement: device
        // ring fill + chunks waiting in the audio channel.
        if rateAdjustEnabled, let controller = rateController {
          let ringFill = playback?.bufferLevel ?? 0
          let queuedFrames = processedChunks.count * chunkSize
          averager.add(Double(ringFill + queuedFrames))

          if stopwatch.elapsedSeconds >= adjustPeriod,
            let avg = averager.average
          {
            let speed = controller.next(level: avg)
            let changed = abs(speed - lastSpeed) > 0.000_001
            stopwatch.restart()
            averager.restart()
            if changed {
              lastSpeed = speed
              if pitchSupported {
                capture?.setPitch(speed)
              } else {
                desiredResamplerRatio.value = speed
              }
              logger.info(
                "Rate adjust: buffer=\(String(format: "%.1f", avg)) target=\(targetLevel) speed=\(String(format: "%.6f", speed)) via \(pitchSupported ? "pitch" : "resampler")"
              )
            } else {
              logger.debug(
                "Rate adjust: buffer=\(String(format: "%.1f", avg)), keeping speed=\(String(format: "%.6f", lastSpeed))"
              )
            }
          }
        }

        do {
          try playback?.write(chunk: chunk)
        } catch {
          logger.error("Playback error: \(error)")
          stop(reason: .playbackError("\(error)"))
          return
        }
      }
    }
    logger.info("Playback thread stopped")
  }

  // MARK: - Helpers

  private func setRealtimePriority() {
    var policy = thread_time_constraint_policy_data_t(
      period: 0,
      computation: UInt32(5_000_000),  // 5ms
      constraint: UInt32(10_000_000),  // 10ms
      preemptible: 1
    )
    let thread = mach_thread_self()
    _ = withUnsafeMutablePointer(to: &policy) { ptr in
      ptr.withMemoryRebound(to: integer_t.self, capacity: Int(THREAD_TIME_CONSTRAINT_POLICY_COUNT))
      { intPtr in
        thread_policy_set(
          thread, UInt32(THREAD_TIME_CONSTRAINT_POLICY),
          intPtr, THREAD_TIME_CONSTRAINT_POLICY_COUNT)
      }
    }
  }

  private func createCaptureBackend(
    config: CamillaDSPConfig, captureRate: Int, chunkSize: Int
  ) throws -> CaptureBackend {
    let captureConfig = config.devices.capture
    switch captureConfig.type {
    case .coreAudio:
      return CoreAudioCapture(
        config: captureConfig, sampleRate: captureRate, chunkSize: chunkSize)
    }
  }

  private func createPlaybackBackend(config: CamillaDSPConfig, chunkSize: Int) throws
    -> PlaybackBackend
  {
    let playbackConfig = config.devices.playback
    let sr = config.devices.samplerate
    switch playbackConfig.type {
    case .coreAudio:
      return CoreAudioPlayback(
        config: playbackConfig, sampleRate: sr, chunkSize: chunkSize)
    }
  }

  private func createResampler(
    config: ResamplerConfig, inputRate: Int, outputRate: Int, channels: Int
  ) -> AudioResampler {
    let chunkSize = currentConfig.devices.chunksize
    switch config.type {
    case .asyncSinc:
      return AsyncSincResampler(
        channels: channels, inputRate: inputRate, outputRate: outputRate,
        profile: config.profile ?? .balanced, chunkSize: chunkSize)
    case .asyncPoly:
      return AsyncPolyResampler(
        channels: channels, inputRate: inputRate, outputRate: outputRate,
        interpolation: .cubic, chunkSize: chunkSize)
    case .synchronous:
      return SynchronousResampler(
        channels: channels, inputRate: inputRate, outputRate: outputRate,
        chunkSize: chunkSize)
    }
  }
}

// MARK: - Thread-safety helpers

private let THREAD_TIME_CONSTRAINT_POLICY_COUNT = mach_msg_type_number_t(
  MemoryLayout<thread_time_constraint_policy_data_t>.size / MemoryLayout<integer_t>.size
)

// MARK: - Silence counter

/// Counts consecutive silent chunks against a dB threshold. Returns
/// `.paused` once the silence has persisted for at least the configured
/// timeout, `.running` otherwise. Direct port of the upstream
/// `SilenceCounter` in `camilladsp/src/utils/countertimer.rs`.
///
/// Disabled when `timeoutSeconds <= 0` — `update` always returns
/// `.running`. This matches the upstream "feature off" behaviour.
private struct SilenceCounter {
  private let limitChunks: Int
  private let thresholdDb: Double
  private var silentChunks: Int = 0

  init(thresholdDb: Double, timeoutSeconds: Double, samplerate: Int, chunksize: Int) {
    self.thresholdDb = thresholdDb
    if timeoutSeconds > 0, chunksize > 0 {
      self.limitChunks = Int((timeoutSeconds * Double(samplerate) / Double(chunksize)).rounded())
    } else {
      self.limitChunks = 0
    }
  }

  /// Feed the next chunk's loudest channel peak (dB). Returns the
  /// engine state the capture loop should drive to.
  mutating func update(signalPeakDb: Double) -> ProcessingState {
    guard limitChunks > 0 else { return .running }
    if signalPeakDb > thresholdDb {
      silentChunks = 0
      return .running
    }
    if silentChunks < limitChunks {
      silentChunks += 1
    }
    return silentChunks >= limitChunks ? .paused : .running
  }
}
