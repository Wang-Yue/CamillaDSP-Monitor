import Accelerate
import Foundation

/// Result of an FFT spectrum query — bin-center frequencies (Hz) and
/// magnitudes (dBFS).
public struct SpectrumResult: Sendable {
  public let frequencies: [Float]
  public let magnitudes: [Float]
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
public final class SpectrumAnalyzer: @unchecked Sendable {
  private let fftN: Int = 4096
  private let log2n: vDSP_Length
  private let fftSetup: FFTSetup
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

  public init() {
    let n = 4096
    let log2nVal = vDSP_Length(log2(Double(n)))
    self.log2n = log2nVal
    guard let setup = vDSP_create_fftsetup(log2nVal, FFTRadix(kFFTRadix2)) else {
      fatalError("Failed to create FFT setup for SpectrumAnalyzer")
    }
    self.fftSetup = setup

    var w = [Float](repeating: 0, count: n)
    vDSP_hann_window(&w, vDSP_Length(n), 0)
    self.window = w

    self.data = [Float](repeating: 0, count: n)
    self.realp = [Float](repeating: 0, count: n / 2)
    self.imagp = [Float](repeating: 0, count: n / 2)
    self.magnitudes = [Float](repeating: 0, count: n / 2 + 1)
    self.dbMagnitudes = [Float](repeating: 0, count: n / 2 + 1)
  }

  deinit {
    vDSP_destroy_fftsetup(fftSetup)
  }

  /// Compute a spectrum on demand (consumer side).
  public func compute(
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

    // 2. Perform FFT using reusable split-complex buffers
    try self.data.withUnsafeBytes { inputPtr in
      guard let complexBase = inputPtr.bindMemory(to: DSPComplex.self).baseAddress else {
        throw SpectrumError.invalidParameters("FFT input bytes have no base address")
      }
      try self.realp.withUnsafeMutableBufferPointer { realPtr in
        guard let realBase = realPtr.baseAddress else {
          throw SpectrumError.invalidParameters("FFT real-part buffer has no base address")
        }
        try self.imagp.withUnsafeMutableBufferPointer { imagPtr in
          guard let imagBase = imagPtr.baseAddress else {
            throw SpectrumError.invalidParameters("FFT imag-part buffer has no base address")
          }
          var splitComplex = DSPSplitComplex(realp: realBase, imagp: imagBase)
          vDSP_ctoz(complexBase, 2, &splitComplex, 1, vDSP_Length(fftN / 2))
          vDSP_fft_zrip(fftSetup, &splitComplex, 1, log2n, FFTDirection(kFFTDirection_Forward))
        }
      }
    }

    // 3. Compute magnitudes in dB directly into preallocated arrays
    let scale = 2.0 / Float(fftN)
    // DC bin
    self.magnitudes[0] = abs(self.realp[0]) * (1.0 / Float(fftN))
    // Nyquist bin (packed in imagp[0])
    self.magnitudes[fftN / 2] = abs(self.imagp[0]) * (1.0 / Float(fftN))
    // All other bins
    for i in 1..<(fftN / 2) {
      self.magnitudes[i] =
        sqrt(self.realp[i] * self.realp[i] + self.imagp[i] * self.imagp[i]) * scale
    }

    // Convert to dB without allocating new arrays via map
    let floorVal: Float = 1e-10
    for i in 0..<self.magnitudes.count {
      self.dbMagnitudes[i] = 20.0 * log10(max(self.magnitudes[i], floorVal))
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
    for i in 0..<nBins {
      let range = plan.ranges[i]
      var maxVal: Float = -200.0
      var count = 0
      for k in max(0, range.lowK)..<min(fftN / 2 + 1, range.highK) {
        maxVal = max(maxVal, self.dbMagnitudes[k])
        count += 1
      }

      if count > 0 {
        outMags[i] = maxVal
      } else {
        let k = max(0, min(fftN / 2, range.nearestK))
        outMags[i] = self.dbMagnitudes[k]
      }
    }

    return SpectrumResult(frequencies: plan.frequencies, magnitudes: outMags)
  }
}
