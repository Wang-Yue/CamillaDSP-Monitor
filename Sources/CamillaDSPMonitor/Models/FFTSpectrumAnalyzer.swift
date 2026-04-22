// FFTSpectrumAnalyzer - FFT spectrum analysis using Accelerate

import Accelerate
import CamillaDSPLib
import Foundation

/// A Swift 6 thread-safe spectrum analyzer.
/// All mutable processing state is confined to a private Task.
/// Results are yielded via an AsyncStream for safe asynchronous delivery to the UI.
final class FFTSpectrumAnalyzer: Sendable {
  /// A stream of frequency band results.
  let results: AsyncStream<[Double]>
  
  private let processingTask: Task<Void, Never>

  static let centerFrequencies: [Double] = [
    25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600,
    2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000,
  ]

  init(sampleRate: Int, chunkSize: Int, ringBuffer: AudioRingBuffer) {
    let (stream, continuation) = AsyncStream.makeStream(of: [Double].self)
    self.results = stream
    
    self.processingTask = Task.detached(priority: .utility) {
      var fftN = 4096
      if sampleRate < 16000 { fftN = 2048 }
      if sampleRate > 48000 { fftN = 8192 }
      if sampleRate > 96000 { fftN = 16384 }
      while fftN < chunkSize { fftN *= 2 }
      
      let log2n = vDSP_Length(log2(Double(fftN)))
      let fftSetup = vDSP_create_fftsetup(log2n, FFTRadix(kFFTRadix2))!
      defer { vDSP_destroy_fftsetup(fftSetup) }

      var window = [Float](repeating: 0, count: fftN)
      vDSP_hann_window(&window, vDSP_Length(fftN), Int32(vDSP_HANN_NORM))

      let halfN = fftN / 2
      var windowed = [Float](repeating: 0, count: fftN)
      var realp = [Float](repeating: 0, count: halfN)
      var imagp = [Float](repeating: 0, count: halfN)
      var magnitudes = [Float](repeating: 0, count: halfN)

      let binWidth = Double(sampleRate) / Double(fftN)
      let factor = pow(2.0, 1.0 / 6.0)
      var bandBins: [(lo: Int, hi: Int)] = []
      for freq in Self.centerFrequencies {
        let fLo = freq / factor
        let fHi = freq * factor
        let binLo = max(1, Int(fLo / binWidth))
        let binHi = min(fftN / 2 - 1, Int(fHi / binWidth))
        bandBins.append((lo: binLo, hi: max(binLo, binHi)))
      }
      
      while !Task.isCancelled {
        try? await Task.sleep(nanoseconds: 100_000_000) // 10Hz
        
        let readCount = windowed.withUnsafeMutableBufferPointer { ptr in
          ringBuffer.readLatest(count: fftN, into: ptr)
        }
        
        guard readCount == fftN else { continue }

        vDSP_vmul(windowed, 1, window, 1, &windowed, 1, vDSP_Length(fftN))

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

        var normScale: Float = 2.0 / Float(fftN)
        vDSP_vsmul(magnitudes, 1, &normScale, &magnitudes, 1, vDSP_Length(halfN))

        var newPeaks = [Double](repeating: -100, count: SPECTRUM_BAND_COUNT)
        for i in 0..<min(bandBins.count, SPECTRUM_BAND_COUNT) {
          let (lo, hi) = bandBins[i]
          var peakMag: Float = 0.0
          for bin in lo...hi {
            if bin < halfN && magnitudes[bin] > peakMag { peakMag = magnitudes[bin] }
          }
          newPeaks[i] = PrcFmt.toDB(peakMag)
        }
        
        continuation.yield(newPeaks)
      }
      
      continuation.finish()
    }
  }

  deinit {
    processingTask.cancel()
  }
}
