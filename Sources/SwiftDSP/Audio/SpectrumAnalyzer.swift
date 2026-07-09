import Accelerate
import Foundation

/// Result of an FFT spectrum query — bin-center frequencies (Hz) and
/// magnitudes (dBFS).
struct SpectrumResult: Sendable {
  let frequencies: [Float]
  let magnitudes: [Float]
}

/// Errors raised by `SpectrumAnalyzer.compute(...)`. The spectrum
/// analyzer wraps an `AudioHistoryBuffer`; channel-out-of-range errors
/// surface as `AudioHistoryBufferError` and bubble through unchanged.
internal enum SpectrumError: Error, Sendable, CustomStringConvertible {
  /// Not enough samples buffered yet to fill an FFT window.
  case bufferEmpty
  /// Caller passed nonsensical FFT parameters.
  case invalidParameters(String)

  internal var description: String {
    switch self {
    case .bufferEmpty: return "Spectrum buffer is empty"
    case .invalidParameters(let msg): return "Invalid spectrum parameters: \(msg)"
    }
  }
}

/// Pure spectrum analyzer that operates on an `AudioHistoryBuffer`.
final class SpectrumAnalyzer {
  private let fftN: Int = 4096
  private let fft: GenericRealFFT<Float>
  private let window: [Float]

  // Preallocated reusable scratch buffers to eliminate frame-by-frame allocations
  private var data: [Float]
  private var realp: [Float]
  private var imagp: [Float]
  private var magnitudes: [Float]
  private var dbMagnitudes: [Float]

  // Cached plan for geometric binning to eliminate transcendental operations
  private struct BinningPlan {
    let minFreq: Double
    let maxFreq: Double
    let nBins: Int
    let samplerate: Int
    let frequencies: [Float]
    let ranges: [(lowK: Int, highK: Int, nearestK: Int)]
  }
  private var binningPlan: BinningPlan?

  init() {
    let n = 4096
    self.fft = GenericRealFFT<Float>(length: n)

    var w = [Float](repeating: 0, count: n)
    vDSP_hann_window(&w, vDSP_Length(n), 0)
    self.window = w

    self.data = [Float](repeating: 0, count: n)
    self.realp = [Float](repeating: 0, count: n / 2 + 1)
    self.imagp = [Float](repeating: 0, count: n / 2 + 1)
    self.magnitudes = [Float](repeating: 0, count: n / 2 + 1)
    self.dbMagnitudes = [Float](repeating: 0, count: n / 2 + 1)
  }

  /// Compute a spectrum on demand (consumer side).
  func compute(
    buffer: AudioHistoryBuffer,
    channel: Int?,
    minFreq: Double,
    maxFreq: Double,
    nBins: Int,
    samplerate: Int
  ) throws -> SpectrumResult {
    guard nBins > 0 else {
      throw SpectrumError.invalidParameters("nBins must be positive")
    }
    guard minFreq > 0 else {
      throw SpectrumError.invalidParameters("minFreq must be positive")
    }
    guard maxFreq > minFreq else {
      throw SpectrumError.invalidParameters("maxFreq must be greater than minFreq")
    }

    // Read data from history buffer directly into preallocated instance buffer
    let success = try self.data.withUnsafeMutableBufferPointer { ptr in
      guard let base = ptr.baseAddress else {
        throw SpectrumError.invalidParameters("FFT input buffer has no base address")
      }
      return try buffer.readLatest(into: base, count: fftN, channel: channel)
    }
    guard success else {
      throw SpectrumError.bufferEmpty
    }

    // 1. Apply Hann window in-place
    vDSP.multiply(self.data, self.window, result: &self.data)

    // 2. Perform FFT using unified RealFFT
    fft.forward(realIn: self.data, specRe: &self.realp, specIm: &self.imagp)

    // 3. Compute magnitudes in dB directly into preallocated arrays
    let scale = 2.0 / Float(fftN)
    let halfN = fftN / 2
    let floorVal: Float = 1e-10

    try self.realp.withUnsafeMutableBufferPointer { realPtr in
      guard let realBase = realPtr.baseAddress else {
        throw SpectrumError.invalidParameters("FFT real-part buffer has no base address")
      }
      try self.imagp.withUnsafeMutableBufferPointer { imagPtr in
        guard let imagBase = imagPtr.baseAddress else {
          throw SpectrumError.invalidParameters("FFT imag-part buffer has no base address")
        }
        try self.magnitudes.withUnsafeMutableBufferPointer { magPtr in
          guard let magBase = magPtr.baseAddress else {
            throw SpectrumError.invalidParameters("Magnitudes buffer has no base address")
          }
          try self.dbMagnitudes.withUnsafeMutableBufferPointer { dbPtr in
            guard let dbBase = dbPtr.baseAddress else {
              throw SpectrumError.invalidParameters("dbMagnitudes buffer has no base address")
            }

            // Calculate magnitudes of all complex bins [0 .. halfN] via vector absolute value
            var splitComplex = DSPSplitComplex(realp: realBase, imagp: imagBase)
            vDSP_zvabs(&splitComplex, 1, magBase, 1, vDSP_Length(halfN + 1))

            // Scale all bins (bins 1..halfN-1 represent conjugate pairs, so they are doubled)
            var scaleAll = scale
            vDSP_vsmul(magBase, 1, &scaleAll, magBase, 1, vDSP_Length(halfN + 1))

            // Correct scaling for DC and Nyquist (they are not doubled, so scale by 1.0/N instead of 2.0/N)
            magBase[0] *= 0.5
            magBase[halfN] *= 0.5

            // Threshold the entire magnitudes array to floorVal in-place
            var nonConstFloor = floorVal
            vDSP_vthr(magBase, 1, &nonConstFloor, magBase, 1, vDSP_Length(halfN + 1))

            // Convert the entire magnitudes array to decibels (dBFS)
            var ref: Float = 1.0
            vDSP_vdbcon(magBase, 1, &ref, dbBase, 1, vDSP_Length(halfN + 1), 1)
          }
        }
      }
    }

    // 4. Geometric Binning via Cached Plan
    let plan: BinningPlan
    if let existing = self.binningPlan,
      existing.minFreq == minFreq,
      existing.maxFreq == maxFreq,
      existing.nBins == nBins,
      existing.samplerate == samplerate
    {
      plan = existing
    } else {
      var outFreqs = [Float](repeating: 0, count: nBins)
      var ranges: [(lowK: Int, highK: Int, nearestK: Int)] = []
      ranges.reserveCapacity(nBins)

      let logMin = log10(minFreq)
      let logMax = log10(maxFreq)
      let step = nBins > 1 ? (logMax - logMin) / Double(nBins - 1) : 0.0

      for i in 0..<nBins {
        let centerLog = logMin + step * Double(i)
        let centerF = pow(10.0, centerLog)
        outFreqs[i] = Float(centerF)

        let lowLog = i > 0 ? centerLog - step / 2 : logMin
        let highLog = i < nBins - 1 ? centerLog + step / 2 : logMax

        let lowF = pow(10.0, lowLog)
        let highF = pow(10.0, highLog)

        let lowK = Int(floor(lowF * Double(fftN) / Double(samplerate)))
        let highK = Int(ceil(highF * Double(fftN) / Double(samplerate)))
        let nearestK = Int(round(centerF * Double(fftN) / Double(samplerate)))

        ranges.append((lowK: lowK, highK: highK, nearestK: nearestK))
      }
      plan = BinningPlan(
        minFreq: minFreq,
        maxFreq: maxFreq,
        nBins: nBins,
        samplerate: samplerate,
        frequencies: outFreqs,
        ranges: ranges
      )
      self.binningPlan = plan
    }

    var outMags = [Float](repeating: 0, count: nBins)
    try self.dbMagnitudes.withUnsafeBufferPointer { dbPtr in
      guard let dbBase = dbPtr.baseAddress else {
        throw SpectrumError.invalidParameters("dbMagnitudes buffer has no base address")
      }
      for i in 0..<nBins {
        let range = plan.ranges[i]
        let start = max(0, range.lowK)
        let end = min(fftN / 2 + 1, range.highK)
        let len = end - start

        if len > 0 {
          var maxVal: Float = -200.0
          vDSP_maxv(dbBase + start, 1, &maxVal, vDSP_Length(len))
          outMags[i] = maxVal
        } else {
          let k = max(0, min(fftN / 2, range.nearestK))
          outMags[i] = dbBase[k]
        }
      }
    }

    return SpectrumResult(frequencies: plan.frequencies, magnitudes: outMags)
  }
}
