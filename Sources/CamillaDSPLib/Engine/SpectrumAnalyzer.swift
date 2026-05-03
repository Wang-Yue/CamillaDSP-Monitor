// FFT spectrum analyzer matching CamillaDSP 4.2.0's `spectrum.rs` semantics:
//   - Hann window cached per FFT length.
//   - Real FFT via Accelerate's packed-real `vDSP_fft_zrip`.
//   - Power spectrum normalised by the squared window sum.
//   - Logarithmic output bins via geometric midpoints with peak-picking
//     across the spanned linear bins.
//   - dBFS reference: a full-scale sine peaks near 0 dBFS.

import Accelerate
import Foundation

/// Result of an FFT spectrum query — bin-center frequencies (Hz) and
/// magnitudes (dBFS).
public struct SpectrumResult: Sendable {
  public let frequencies: [Float]
  public let magnitudes: [Float]
}

public enum SpectrumError: Error, Sendable {
  case bufferEmpty
  case invalidParameters(String)
  case channelOutOfRange(channel: Int, available: Int)
}

// MARK: - FFT computer with preallocated scratch

/// Cached FFT setup, Hann window, and consumer-side scratch buffers
/// keyed on FFT length. Each entry is allocated once on first request
/// and reused on every subsequent query at the same length.
final class SpectrumComputer {
  private final class Cached {
    let fftLen: Int
    let halfLen: Int
    let log2n: vDSP_Length
    let setup: FFTSetup
    let hann: UnsafeMutableBufferPointer<Float>
    let invWindowSumSquared: Float

    // Consumer-side scratch — mutated in place during `powerSpectrum`.
    // The actor serialises consumers, so a single scratch per length
    // is enough.
    let samples: UnsafeMutableBufferPointer<Float>  // raw input window
    let windowed: UnsafeMutableBufferPointer<Float>  // samples × hann
    let realParts: UnsafeMutableBufferPointer<Float>  // split-complex re
    let imagParts: UnsafeMutableBufferPointer<Float>  // split-complex im
    let power: UnsafeMutableBufferPointer<Float>  // |X|² normalised

    init(fftLen: Int) throws {
      precondition(
        fftLen >= 2 && (fftLen & (fftLen - 1)) == 0,
        "FFT length must be a power of two ≥ 2")
      self.fftLen = fftLen
      self.halfLen = fftLen / 2
      self.log2n = vDSP_Length(log2(Double(fftLen)).rounded())

      guard let setup = vDSP_create_fftsetup(self.log2n, FFTRadix(kFFTRadix2)) else {
        throw SpectrumError.invalidParameters("Failed to create FFT setup")
      }
      self.setup = setup

      // Hann window: w[i] = 0.5 * (1 - cos(2π·i/(n-1))). Computed in
      // Double and stored as Float to match the FFT precision.
      let hann = UnsafeMutableBufferPointer<Float>.allocate(capacity: fftLen)
      let denom = Double(fftLen - 1)
      for i in 0..<fftLen {
        hann[i] = Float(0.5 * (1.0 - cos(2.0 * .pi * Double(i) / denom)))
      }
      self.hann = hann
      let windowSum = vDSP.sum(UnsafeBufferPointer(hann))
      self.invWindowSumSquared = 1.0 / (windowSum * windowSum)

      self.samples = .allocate(capacity: fftLen)
      self.windowed = .allocate(capacity: fftLen)
      self.realParts = .allocate(capacity: halfLen)
      self.imagParts = .allocate(capacity: halfLen)
      self.power = .allocate(capacity: halfLen + 1)
      samples.initialize(repeating: 0)
      windowed.initialize(repeating: 0)
      realParts.initialize(repeating: 0)
      imagParts.initialize(repeating: 0)
      power.initialize(repeating: 0)
    }

    deinit {
      vDSP_destroy_fftsetup(setup)
      hann.deallocate()
      samples.deallocate()
      windowed.deallocate()
      realParts.deallocate()
      imagParts.deallocate()
      power.deallocate()
    }
  }

  private var cache: [Int: Cached] = [:]

  /// Returns a cached `Cached` instance for `fftLen`, allocating on
  /// first miss. Consumer-side; assumes single-threaded access via the
  /// owning actor.
  private func entry(for fftLen: Int) throws -> Cached {
    if let existing = cache[fftLen] { return existing }
    let fresh = try Cached(fftLen: fftLen)
    cache[fftLen] = fresh
    return fresh
  }

  /// Reads `fftLen` samples from `buffer` into the cached scratch and
  /// computes the single-sided power spectrum. Returns the cached
  /// `power` buffer (length `halfLen + 1`); it is overwritten on every
  /// call so callers should consume immediately. Returns `nil` if the
  /// ring buffer doesn't have enough samples yet.
  func computePower(
    samplesFor fftLen: Int,
    from buffer: AudioHistoryBuffer,
    channel: Int?
  ) throws -> UnsafeBufferPointer<Float>? {
    let e = try entry(for: fftLen)

    // 1) Pull the latest `fftLen` samples into the input scratch.
    guard let samplesPtr = e.samples.baseAddress else { return nil }
    let ok = try buffer.readLatest(
      into: samplesPtr,
      count: fftLen,
      channel: channel)
    guard ok else { return nil }

    // 2) Apply the Hann window: windowed[i] = samples[i] * hann[i].
    //    `vDSP.multiply` takes `result` `inout`, so we need a `var`
    //    binding for the buffer pointer (the memory it points to is
    //    already writable).
    var windowedRef = e.windowed
    vDSP.multiply(
      UnsafeBufferPointer(e.samples),
      UnsafeBufferPointer(e.hann),
      result: &windowedRef)

    // 3) Pack the real signal into split-complex form, then run the
    //    in-place packed-real FFT. The C API (`vDSP_fft_zrip`) is
    //    still the canonical entry point for packed-real transforms;
    //    the Swift `vDSP.FFT<DSPSplitComplex>` wrapper does ordinary
    //    complex-to-complex via `vDSP_fft_zip` and would require
    //    separate twiddle post-processing.
    let halfLen = e.halfLen
    guard let realPtr = e.realParts.baseAddress else { return nil }
    guard let imagPtr = e.imagParts.baseAddress else { return nil }
    guard let windowedPtr = e.windowed.baseAddress else { return nil }
    var split = DSPSplitComplex(realp: realPtr, imagp: imagPtr)
    windowedPtr.withMemoryRebound(to: DSPComplex.self, capacity: halfLen) {
      complexPtr in
      vDSP_ctoz(complexPtr, 2, &split, 1, vDSP_Length(halfLen))
    }
    vDSP_fft_zrip(e.setup, &split, 1, e.log2n, FFTDirection(FFT_FORWARD))

    // 4) Compute the single-sided power spectrum. Unlike Rust's
    //    `realfft` which returns textbook coefficients, vDSP already
    //    pre-doubles interior bins for one-sided amplitude, so the
    //    Rust ×4 power scale collapses to ×1 here. DC and Nyquist
    //    are stored un-doubled in rp[0] / ip[0] and use ×1 too.
    let invW2 = e.invWindowSumSquared
    let rp = e.realParts
    let ip = e.imagParts
    let pp = e.power
    pp[0] = (rp[0] * rp[0]) * invW2
    pp[halfLen] = (ip[0] * ip[0]) * invW2
    for k in 1..<halfLen {
      let re = rp[k]
      let im = ip[k]
      pp[k] = (re * re + im * im) * invW2
    }
    return UnsafeBufferPointer(pp)
  }
}

// MARK: - FFT length policy + log-bin aggregation

/// FFT length policy: pick the smallest power-of-two that holds at least one
/// full period at `minFreq`, capped at the ring-buffer capacity. Mirrors
/// `fft_length_for` in the Rust upstream.
func fftLengthFor(minFreq: Double, samplerate: Int) -> Int {
  guard minFreq > 0 else { return kRingBufferCapacity }
  let needed = max(2.0, ceil(Double(samplerate) / minFreq))
  var n = 2
  while n < Int(needed) && n < kRingBufferCapacity {
    n <<= 1
  }
  return Swift.min(n, kRingBufferCapacity)
}

/// Convert a linear power array into log-spaced bin magnitudes (dBFS).
/// Each output bin's magnitude is the *peak* power across the linear FFT
/// bins that fall inside its geometric edges — same algorithm as Rust's
/// `aggregate_log_bins`.
func logBinMagnitudes(
  power: UnsafeBufferPointer<Float>,
  fftLen: Int,
  samplerate: Int,
  minFreq: Double,
  maxFreq: Double,
  nBins: Int
) -> SpectrumResult {
  guard nBins >= 1, minFreq > 0, maxFreq > minFreq, fftLen >= 2 else {
    return SpectrumResult(frequencies: [], magnitudes: [])
  }
  let freqRes = Double(samplerate) / Double(fftLen)
  let halfLen = fftLen / 2

  var frequencies = [Float](repeating: 0, count: nBins)
  let logRatio: Double = nBins > 1 ? pow(maxFreq / minFreq, 1.0 / Double(nBins - 1)) : 1.0
  for i in 0..<nBins {
    frequencies[i] = Float(minFreq * pow(logRatio, Double(i)))
  }

  let halfStep = sqrt(logRatio)
  var magnitudes = [Float](repeating: 0, count: nBins)
  let floor: Float = 1e-30  // Same noise floor as Rust to avoid log(0).

  for i in 0..<nBins {
    let centre = Double(frequencies[i])
    let lowEdge: Double = (i == 0) ? minFreq : centre / halfStep
    let highEdge: Double = (i == nBins - 1) ? maxFreq : centre * halfStep

    let kLow = Swift.max(0, Int(Foundation.floor(lowEdge / freqRes)))
    let kHigh = Swift.min(halfLen, Int(ceil(highEdge / freqRes)))

    var peak: Float = floor
    if kLow < kHigh {
      for k in kLow..<kHigh where power[k] > peak {
        peak = power[k]
      }
    } else {
      let kNearest = Swift.max(
        0,
        Swift.min(
          halfLen,
          Int((centre / freqRes).rounded())))
      peak = Swift.max(power[kNearest], floor)
    }
    magnitudes[i] = 10.0 * log10f(peak)
  }
  return SpectrumResult(frequencies: frequencies, magnitudes: magnitudes)
}

// Convenience overload that takes an `Array<Float>` so the existing
// `SpectrumAnalyzerTests.testLogBinFrequenciesAreGeometric` keeps working
// without exposing `UnsafeBufferPointer` to test code.
func logBinMagnitudes(
  power: [Float],
  fftLen: Int,
  samplerate: Int,
  minFreq: Double,
  maxFreq: Double,
  nBins: Int
) -> SpectrumResult {
  power.withUnsafeBufferPointer { ptr in
    logBinMagnitudes(
      power: ptr,
      fftLen: fftLen,
      samplerate: samplerate,
      minFreq: minFreq,
      maxFreq: maxFreq,
      nBins: nBins)
  }
}

// MARK: - Owner

/// Pure spectrum analyzer that operates on an `AudioHistoryBuffer`.
public final class SpectrumAnalyzer {
  let computer = SpectrumComputer()

  public init() {}

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
    guard buffer.hasData else { throw SpectrumError.bufferEmpty }

    let fftLen = fftLengthFor(minFreq: minFreq, samplerate: samplerate)
    guard
      let power = try computer.computePower(
        samplesFor: fftLen, from: buffer, channel: channel
      )
    else {
      // Buffer hasn't accumulated `fftLen` samples yet — return a
      // flat noise floor so the UI can render a quiet trace during
      // engine start-up.
      return SpectrumResult(
        frequencies: logBinFrequencies(
          minFreq: minFreq,
          maxFreq: maxFreq,
          nBins: nBins),
        magnitudes: Array(repeating: -300.0, count: nBins)
      )
    }
    return logBinMagnitudes(
      power: power,
      fftLen: fftLen,
      samplerate: samplerate,
      minFreq: minFreq,
      maxFreq: maxFreq,
      nBins: nBins)
  }

  private func logBinFrequencies(minFreq: Double, maxFreq: Double, nBins: Int) -> [Float] {
    guard nBins > 0 else { return [] }
    var out = [Float](repeating: 0, count: nBins)
    if nBins == 1 {
      out[0] = Float(minFreq)
    } else {
      let ratio = pow(maxFreq / minFreq, 1.0 / Double(nBins - 1))
      for i in 0..<nBins {
        out[i] = Float(minFreq * pow(ratio, Double(i)))
      }
    }
    return out
  }
}
