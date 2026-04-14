// FFTSpectrumAnalyzer - 30-band FFT spectrum analysis using Accelerate

import Accelerate
import CamillaDSPLib
import Foundation

/// Thread-safe reference holder for FFTSpectrumAnalyzer.
/// Used by the audio tap callback to call enqueueAudio() without going through
/// @MainActor-isolated AppState properties (which would crash on the audio thread).
final class AnalyzerRef: @unchecked Sendable {
  private let lock = NSLock()
  private var _analyzer: FFTSpectrumAnalyzer?
  var analyzer: FFTSpectrumAnalyzer? {
    get {
      lock.lock()
      defer { lock.unlock() }
      return _analyzer
    }
    set {
      lock.lock()
      defer { lock.unlock() }
      _analyzer = newValue
    }
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
  private var timerPaused = false

  // Pre-allocated Float buffers
  private var windowed: [Float]
  private var realp: [Float]
  private var imagp: [Float]
  private var magnitudes: [Float]

  static let centerFrequencies: [Double] = [
    25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600,
    2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000,
  ]

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
    for freq in Self.centerFrequencies {
      let fLo = freq / factor
      let fHi = freq * factor
      let binLo = max(1, Int(fLo / binWidth))
      let binHi = min(fftN / 2 - 1, Int(fHi / binWidth))
      bins.append((lo: binLo, hi: max(binLo, binHi)))
    }
    self.bandBins = bins
    self.bandPeaks = Array(repeating: -100, count: bandCount)
    let timer = DispatchSource.makeTimerSource(queue: spectrumQueue)
    timer.schedule(deadline: .now(), repeating: .milliseconds(100))
    timer.setEventHandler { [weak self] in self?.processLatestBuffer() }
    timer.resume()
    self.spectrumTimer = timer
  }

  deinit {
    // Must resume before cancel to avoid crash on dealloc of a suspended DispatchSource.
    if timerPaused { spectrumTimer?.resume() }
    spectrumTimer?.cancel()
    vDSP_destroy_fftsetup(fftSetup)
  }

  /// Pause FFT computation. readBands() still returns the last computed result.
  /// Each pause() must be balanced by exactly one resume().
  func pause() {
    guard !timerPaused else { return }
    timerPaused = true
    spectrumTimer?.suspend()
  }

  /// Resume FFT computation after a pause().
  func resume() {
    guard timerPaused else { return }
    timerPaused = false
    spectrumTimer?.resume()
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
