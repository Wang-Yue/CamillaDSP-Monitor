// SpectrumEngine - Shared CoreAudio tap + FFT lifecycle driven by app state

import Foundation
import Observation

@MainActor
@Observable
final class SpectrumEngine {
  private(set) var bands: [Double] = Array(repeating: -100, count: SPECTRUM_BAND_COUNT)

  private let dsp: DSPEngineController
  private let devices: AudioDeviceManager
  private let settings: AudioSettings

  private var analyzer: FFTSpectrumAnalyzer?
  private var tap: CoreAudioTap?
  private var updateTask: Task<Void, Never>?
  private var transitionTask: Task<Void, Never>?
  private var currentTapConfig: TapConfig?

  init(dsp: DSPEngineController, devices: AudioDeviceManager, settings: AudioSettings) {
    self.dsp = dsp
    self.devices = devices
    self.settings = settings
  }

  func refresh() {
    refreshLifecycle(
      status: dsp.status, captureConfig: devices.captureConfig, chunkSize: settings.chunkSize)
  }

  private func refreshLifecycle(status: AppStatus, captureConfig: DeviceConfig, chunkSize: Int) {
    let nextConfig = TapConfig(
      sampleRate: captureConfig.sampleRate,
      chunkSize: chunkSize,
      deviceName: captureConfig.deviceName
    )

    let previousTransition = transitionTask
    previousTransition?.cancel()

    transitionTask = Task {
      // Wait for any previous transition to finish its cleanup
      _ = await previousTransition?.result

      // Debounce to let rapid changes settle.
      // If a newer task comes in during this sleep, this one will be cancelled.
      try? await Task.sleep(nanoseconds: 100_000_000)  // 100ms debounce
      guard !Task.isCancelled else { return }

      if status != .running {
        await deactivate()
      } else if analyzer == nil || currentTapConfig != nextConfig {
        await deactivate()
        if !Task.isCancelled {
          activate(with: nextConfig)
        }
      }
    }
  }

  private func activate(with config: TapConfig) {
    guard analyzer == nil else { return }

    let ringBuffer = AudioRingBuffer(capacity: max(config.sampleRate, 16384))

    let newAnalyzer = FFTSpectrumAnalyzer(
      sampleRate: config.sampleRate,
      chunkSize: config.chunkSize,
      ringBuffer: ringBuffer
    )
    analyzer = newAnalyzer

    tap = CoreAudioTap(deviceName: config.deviceName, ringBuffer: ringBuffer)
    currentTapConfig = config

    // Subscribe to analyzer results via AsyncStream
    updateTask = Task {
      for await newBands in newAnalyzer.results {
        if !Task.isCancelled {
          bands = newBands
        }
      }
    }
  }

  private func deactivate() async {
    updateTask?.cancel()
    updateTask = nil

    let oldTap = tap
    let oldAnalyzer = analyzer

    tap = nil
    analyzer = nil
    currentTapConfig = nil

    // Ensure hardware resources are released before continuing
    await oldTap?.stop()
    await oldAnalyzer?.stop()

    bands = Array(repeating: -100, count: SPECTRUM_BAND_COUNT)
  }
}

private struct TapConfig: Equatable {
  let sampleRate: Int
  let chunkSize: Int
  let deviceName: String?
}
