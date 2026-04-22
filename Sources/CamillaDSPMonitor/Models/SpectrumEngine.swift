// SpectrumEngine - Shared CoreAudio tap + FFT lifecycle driven by app state

import Combine
import Foundation

@MainActor
final class SpectrumEngine: ObservableObject {
  @Published private(set) var bands: [Double] = Array(repeating: -100, count: 30)

  private let dsp: DSPEngineController
  private let devices: AudioDeviceManager
  private let settings: AudioSettings

  private var analyzer: FFTSpectrumAnalyzer?
  private var tap: CoreAudioTap?
  private let analyzerRef = AnalyzerRef()
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
    .sink { [weak self] _, _, _ in
      self?.refreshLifecycle()
    }

    refreshLifecycle()
  }

  private func refreshLifecycle() {
    guard dsp.status == .running else {
      deactivate()
      return
    }

    let nextConfig = TapConfig(
      sampleRate: devices.captureConfig.sampleRate,
      chunkSize: settings.chunkSize,
      deviceName: devices.captureConfig.deviceName
    )

    guard analyzer == nil || currentTapConfig != nextConfig else { return }

    deactivate()
    activate(with: nextConfig)
  }

  private func activate(with config: TapConfig) {
    guard analyzer == nil else { return }

    let nextAnalyzer = FFTSpectrumAnalyzer(sampleRate: config.sampleRate, chunkSize: config.chunkSize)
    analyzer = nextAnalyzer
    analyzerRef.analyzer = nextAnalyzer

    let ref = analyzerRef
    let nextTap = CoreAudioTap(onAudio: { waveform in ref.analyzer?.enqueueAudio(waveform) })
    tap = nextTap
    currentTapConfig = config

    Task { await nextTap.start(deviceName: config.deviceName) }

    updateTask = Task {
      while !Task.isCancelled {
        try? await Task.sleep(nanoseconds: 100_000_000)
        guard !Task.isCancelled, let analyzer else { break }
        bands = analyzer.readBands()
      }
    }
  }

  private func deactivate() {
    updateTask?.cancel()
    updateTask = nil

    analyzerRef.analyzer = nil
    analyzer = nil
    currentTapConfig = nil

    let currentTap = tap
    tap = nil
    if let currentTap {
      Task { await currentTap.stop() }
    }

    bands = Array(repeating: -100, count: 30)
  }
}

private struct TapConfig: Equatable {
  let sampleRate: Int
  let chunkSize: Int
  let deviceName: String?
}
