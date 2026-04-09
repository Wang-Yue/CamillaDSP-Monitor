// AppState+Monitoring - Unified FFT spectrum analysis with independent CoreAudio capture

import Accelerate
import AudioToolbox
import CamillaDSPLib
import Foundation

struct StereoLevel: Sendable {
  var left: Double
  var right: Double
  static let silent = StereoLevel(left: -100, right: -100)

  init(left: Double, right: Double) {
    self.left = left
    self.right = right
  }

  init(from levels: [Float]) {
    left = max(-100.0, Double(levels.first ?? -100))
    right = max(-100.0, Double(levels.count > 1 ? levels[1] : -100))
  }
}

@MainActor
final class MeterState: ObservableObject {
  var capturePeak: StereoLevel = .silent
  var captureRms: StereoLevel = .silent
  var playbackPeak: StereoLevel = .silent
  var playbackRms: StereoLevel = .silent
  var spectrumBands: [Double] = Array(repeating: -100, count: 30)
  var processingLoad: Double = 0

  func update(
    capturePeak: StereoLevel, captureRms: StereoLevel, playbackPeak: StereoLevel,
    playbackRms: StereoLevel, spectrumBands: [Double], processingLoad: Double
  ) {
    self.capturePeak = capturePeak
    self.captureRms = captureRms
    self.playbackPeak = playbackPeak
    self.playbackRms = playbackRms
    self.spectrumBands = spectrumBands
    self.processingLoad = processingLoad
    objectWillChange.send()
  }

  func reset() {
    update(
      capturePeak: .silent, captureRms: .silent, playbackPeak: .silent, playbackRms: .silent,
      spectrumBands: Array(repeating: -100, count: 30), processingLoad: 0)
  }
}

extension AppState {

  func recreateSpectrumAnalyzer() {
    spectrumAnalyzer = FFTSpectrumAnalyzer(sampleRate: sampleRate, chunkSize: chunkSize)
  }

  func scheduleSpectrumRestart() {
    guard isRunning else { return }
    spectrumRestartTask?.cancel()
    spectrumRestartTask = Task {
      try? await Task.sleep(nanoseconds: 500_000_000)
      guard !Task.isCancelled else { return }
      recreateSpectrumAnalyzer()
      if audioTap != nil {
        audioTap?.start(deviceName: selectedCaptureDevice)
      } else {
        startAudioCapture()
      }
    }
  }

  func startMonitoringTimer() {
    monitoringTask?.cancel()
    monitoringTask = Task {
      while !Task.isCancelled {
        if status == .running {
          await pollLevels()
        }
        try? await Task.sleep(nanoseconds: 100_000_000)  // 100ms
      }
    }
  }

  func stopMonitoring() {
    monitoringTask?.cancel()
    monitoringTask = nil
    spectrumRestartTask?.cancel()
    spectrumRestartTask = nil
    isPollingLevels = false
    stopAudioCapture()
    spectrumAnalyzer = nil
  }

  private func pollLevels() async {
    guard status == .running && !isPollingLevels else { return }
    isPollingLevels = true
    defer { isPollingLevels = false }

    do {
      let state: String? = try await engine.sendCommand("GetState")

      if state != "Running" && state != "Starting" && state != "Stalled" {
        try? await Task.sleep(nanoseconds: 500_000_000)
        let secondCheck: String? = try? await engine.sendCommand("GetState")
        if secondCheck == "Running" || secondCheck == "Starting" || secondCheck == "Stalled" {
          return
        }
        let reason: String? = try await engine.sendCommand("GetStopReason")
        let stopReason = reason ?? "None"
        if stopReason == "CaptureFormatChange" || stopReason == "None" { return }
        let now = Date()
        if let last = lastRecoveryTime, now.timeIntervalSince(last) < 5.0 { return }
        lastRecoveryTime = now
        lastAppliedConfigYAML = nil
        await applyConfigAsync()
        return
      }

      if state == "Starting" { return }
      guard let levels = await engine.getSignalLevels() else { return }
      let bands = spectrumAnalyzer?.readBands() ?? meters.spectrumBands
      let newLoad = Double(levels.processing_load ?? 0)

      meters.update(
        capturePeak: StereoLevel(from: levels.capture_peak),
        captureRms: StereoLevel(from: levels.capture_rms),
        playbackPeak: StereoLevel(from: levels.playback_peak),
        playbackRms: StereoLevel(from: levels.playback_rms),
        spectrumBands: bands,
        processingLoad: newLoad
      )

    } catch {
      if !(await engine.ping()) {
        status = .error("Connection lost: \(error.localizedDescription)")
        stopMonitoring()
      }
    }
  }

  func startAudioCapture() {
    if audioTap == nil {
      audioTap = CoreAudioTap(onAudio: { [weak self] waveform in
        self?.spectrumAnalyzer?.enqueueAudio(waveform)
      })
    }
    audioTap?.start(deviceName: selectedCaptureDevice)
  }

  func stopAudioCapture() {
    audioTap?.stop()
  }
}

final class FFTSpectrumAnalyzer {
  private let sampleRate: Int
  private let fftSize: Int
  private let log2n: vDSP_Length
  private let fftSetup: FFTSetup
  private let window: [Float]
  private let bandBins: [(lo: Int, hi: Int)]
  private let bandCount = 30
  private let bufferLock = NSLock()
  private var pendingBuffer: [Float]? = nil
  private let resultsLock = NSLock()
  private var bandPeaks: [Double]
  private let spectrumQueue = DispatchQueue(label: "camilladsp.spectrum.fft", qos: .utility)
  private var spectrumTimer: DispatchSourceTimer?

  // Pre-allocated Float buffers
  private var windowed: [Float]
  private var realp: [Float]
  private var imagp: [Float]
  private var magnitudes: [Float]

  init(sampleRate: Int, chunkSize: Int) {
    self.sampleRate = sampleRate
    var fftN = 4096
    if sampleRate > 48000 { fftN = 8192 }
    if sampleRate > 96000 { fftN = 16384 }
    while fftN < chunkSize { fftN *= 2 }
    self.fftSize = fftN
    self.log2n = vDSP_Length(log2(Double(fftN)))
    self.fftSetup = vDSP_create_fftsetup(log2n, FFTRadix(kFFTRadix2))!

    var win = [Float](repeating: 0, count: fftN)
    vDSP_hann_window(&win, vDSP_Length(fftN), Int32(vDSP_HANN_NORM))
    self.window = win

    let halfN = fftN / 2
    self.windowed = [Float](repeating: 0, count: fftN)
    self.realp = [Float](repeating: 0, count: halfN)
    self.imagp = [Float](repeating: 0, count: halfN)
    self.magnitudes = [Float](repeating: 0, count: halfN)

    let binWidth = Double(sampleRate) / Double(fftN)
    let factor = pow(2.0, 1.0 / 6.0)
    var bins: [(lo: Int, hi: Int)] = []
    let centerFrequencies: [Double] = [
      25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600,
      2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000,
    ]
    for freq in centerFrequencies {
      let fLo = freq / factor
      let fHi = freq * factor
      let binLo = max(1, Int(fLo / binWidth))
      let binHi = min(fftN / 2 - 1, Int(fHi / binWidth))
      bins.append((lo: binLo, hi: max(binLo, binHi)))
    }
    self.bandBins = bins
    self.bandPeaks = Array(repeating: -100, count: bandCount)
    let timer = DispatchSource.makeTimerSource(queue: spectrumQueue)
    timer.schedule(deadline: .now(), repeating: .milliseconds(50))
    timer.setEventHandler { [weak self] in self?.processLatestBuffer() }
    timer.resume()
    self.spectrumTimer = timer
  }

  deinit {
    spectrumTimer?.cancel()
    vDSP_destroy_fftsetup(fftSetup)
  }

  func enqueueAudio(_ waveform: [Float]) {
    bufferLock.lock()
    pendingBuffer = waveform
    bufferLock.unlock()
  }

  func readBands() -> [Double] {
    resultsLock.lock()
    let result = bandPeaks
    resultsLock.unlock()
    return result
  }

  private func processLatestBuffer() {
    bufferLock.lock()
    guard let waveform = pendingBuffer else {
      bufferLock.unlock()
      return
    }
    pendingBuffer = nil
    bufferLock.unlock()

    let n = min(waveform.count, fftSize)
    vDSP_vmul(waveform, 1, window, 1, &windowed, 1, vDSP_Length(n))
    if n < fftSize {
      vDSP_vclr(&windowed[n], 1, vDSP_Length(fftSize - n))
    }

    let halfN = fftSize / 2
    realp.withUnsafeMutableBufferPointer { rBuf in
      imagp.withUnsafeMutableBufferPointer { iBuf in
        var split = DSPSplitComplex(realp: rBuf.baseAddress!, imagp: iBuf.baseAddress!)
        windowed.withUnsafeBufferPointer { wBuf in
          vDSP_ctoz(
            UnsafePointer<DSPComplex>(OpaquePointer(wBuf.baseAddress!)), 2, &split, 1,
            vDSP_Length(halfN))
        }
        vDSP_fft_zrip(fftSetup, &split, 1, log2n, FFTDirection(kFFTDirection_Forward))
      }
    }

    realp.withUnsafeBufferPointer { rBuf in
      imagp.withUnsafeBufferPointer { iBuf in
        var split = DSPSplitComplex(
          realp: UnsafeMutablePointer(mutating: rBuf.baseAddress!),
          imagp: UnsafeMutablePointer(mutating: iBuf.baseAddress!))
        vDSP_zvabs(&split, 1, &magnitudes, 1, vDSP_Length(halfN))
      }
    }

    var normScale: Float = 2.0 / Float(fftSize)
    vDSP_vsmul(magnitudes, 1, &normScale, &magnitudes, 1, vDSP_Length(halfN))

    var newPeaks = [Double](repeating: -100, count: bandCount)
    for i in 0..<min(bandBins.count, bandCount) {
      let (lo, hi) = bandBins[i]
      var peakMag: Float = 0.0
      for bin in lo...hi {
        if bin < halfN && magnitudes[bin] > peakMag { peakMag = magnitudes[bin] }
      }
      newPeaks[i] = PrcFmt.toDB(peakMag)
    }
    resultsLock.lock()
    bandPeaks = newPeaks
    resultsLock.unlock()
  }
}
