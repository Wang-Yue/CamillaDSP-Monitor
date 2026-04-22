// SpectrumEngine - Shared CoreAudio tap + FFT lifecycle driven by app state

import Combine
import Foundation

@MainActor
final class SpectrumEngine: ObservableObject {
  @Published private(set) var bands: [Double] = Array(repeating: -100, count: SPECTRUM_BAND_COUNT)

  private let dsp: DSPEngineController
  private let devices: AudioDeviceManager
  private let settings: AudioSettings

  private var analyzer: FFTSpectrumAnalyzer?
  private var tap: CoreAudioTap?
  private var updateTask: Task<Void, Never>?
  private var lifecycleCancellable: AnyCancellable?
  private var currentTapConfig: TapConfig?

  init(dsp: DSPEngineController, devices: AudioDeviceManager, settings: AudioSettings) {
    self.dsp = dsp
    self.devices = devices
    self.settings = settings

    lifecycleCancellable = Publishers.CombineLatest3(
      dsp.$status.removeDuplicates(),
      devices.$captureConfig.removeDuplicates(),
      settings.$chunkSize.removeDuplicates()
    )
    .sink { [weak self] status, captureConfig, chunkSize in
      self?.refreshLifecycle(status: status, captureConfig: captureConfig, chunkSize: chunkSize)
    }
  }

  private func refreshLifecycle(status: AppStatus, captureConfig: DeviceConfig, chunkSize: Int) {
    guard status == .running else {
      deactivate()
      return
    }

    let nextConfig = TapConfig(
      sampleRate: captureConfig.sampleRate,
      chunkSize: chunkSize,
      deviceName: captureConfig.deviceName
    )

    guard analyzer == nil || currentTapConfig != nextConfig else { return }

    deactivate()
    activate(with: nextConfig)
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

  private func deactivate() {
    updateTask?.cancel()
    updateTask = nil

    analyzer = nil
    tap = nil
    currentTapConfig = nil

    bands = Array(repeating: -100, count: SPECTRUM_BAND_COUNT)
  }
}

private struct TapConfig: Equatable {
  let sampleRate: Int
  let chunkSize: Int
  let deviceName: String?
}
