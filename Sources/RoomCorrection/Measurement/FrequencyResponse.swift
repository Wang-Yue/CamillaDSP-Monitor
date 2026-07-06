// Single-sided complex frequency response derived from an
// `ImpulseResponse`.
//
// Stored as separate `real` / `imag` arrays of `bins = N/2 + 1`, where
// `N` is the FFT length used to transform the (zero-padded) IR.
// Magnitude / phase / group-delay / per-bin frequency accessors are
// computed on demand — there's no value in eagerly caching when the
// caller is typically rendering one or two of those views at a time.
//
// All transforms route through `RealFFT` so the FFT length
// can be any even number; callers don't have to round up to a power
// of two themselves.

import Foundation

public struct FrequencyResponse: Sendable {
  public let real: [Double]
  public let imag: [Double]
  public let sampleRate: Int
  /// FFT length used to produce these bins (i.e. the time-domain
  /// signal, zero-padded to this length).
  public let fftSize: Int

  public init(real: [Double], imag: [Double], sampleRate: Int, fftSize: Int) {
    precondition(real.count == imag.count, "FrequencyResponse: re/im length mismatch")
    precondition(fftSize > 0 && fftSize % 2 == 0, "FrequencyResponse: fftSize must be even and > 0")
    precondition(
      real.count == fftSize / 2 + 1, "FrequencyResponse: bin count must equal fftSize/2 + 1")
    self.real = real
    self.imag = imag
    self.sampleRate = sampleRate
    self.fftSize = fftSize
  }

  public var bins: Int { real.count }

  /// Frequency of bin `k` in Hz.
  public func frequency(at bin: Int) -> Double {
    return Double(bin) * Double(sampleRate) / Double(fftSize)
  }

  public func magnitude(at bin: Int) -> Double {
    let r = real[bin]
    let i = imag[bin]
    return (r * r + i * i).squareRoot()
  }

  /// Magnitude in dB FS. Floored at -1000 dB for the zero-magnitude
  /// case (matches the project's `Double.toDB` convention).
  public func magnitudeDB(at bin: Int) -> Double {
    return Double.toDB(magnitude(at: bin))
  }

  /// Wrapped phase in radians, ∈ (−π, π].
  public func phase(at bin: Int) -> Double {
    return atan2(imag[bin], real[bin])
  }

  /// Phase across all bins, unwrapped via the standard
  /// neighbour-difference algorithm. Values are continuous but no
  /// assumption is made about the absolute branch — for group-delay
  /// computations the relative behaviour is what matters.
  public func unwrappedPhase() -> [Double] {
    let n = bins
    if n == 0 { return [] }
    var out = [Double](repeating: 0, count: n)
    out[0] = phase(at: 0)
    for i in 1..<n {
      var diff = phase(at: i) - phase(at: i - 1)
      while diff > Double.pi { diff -= 2.0 * Double.pi }
      while diff < -Double.pi { diff += 2.0 * Double.pi }
      out[i] = out[i - 1] + diff
    }
    return out
  }

  /// Group delay in seconds, computed by finite-difference on the
  /// unwrapped phase: τ_g(f) ≈ −Δφ / Δω. The endpoints reuse the
  /// neighbouring difference (same first-order treatment as `numpy`).
  public func groupDelaySeconds() -> [Double] {
    let phases = unwrappedPhase()
    let n = phases.count
    guard n >= 2 else { return [Double](repeating: 0, count: n) }

    let binHz = Double(sampleRate) / Double(fftSize)
    let twoPi = 2.0 * Double.pi
    var gd = [Double](repeating: 0, count: n)
    // Centred difference where possible.
    for i in 1..<(n - 1) {
      gd[i] = -(phases[i + 1] - phases[i - 1]) / (twoPi * 2.0 * binHz)
    }
    gd[0] = -(phases[1] - phases[0]) / (twoPi * binHz)
    gd[n - 1] = -(phases[n - 1] - phases[n - 2]) / (twoPi * binHz)
    return gd
  }

  /// FFT an `ImpulseResponse` to its frequency response.
  ///
  /// `fftSize` (if provided) must be even and ≥ `ir.count`. The IR is
  /// zero-padded to that length; passing a longer length increases
  /// frequency resolution but does not add information. When
  /// `fftSize` is `nil`, the next even length ≥ `ir.count` is used.
  public static func from(impulseResponse ir: ImpulseResponse, fftSize: Int? = nil)
    -> FrequencyResponse
  {
    func nextPowerOfTwo(_ val: Int) -> Int {
      var p = 8
      while p < val { p *= 2 }
      return p
    }
    let n = nextPowerOfTwo(max(8, fftSize ?? ir.count))
    let bins = n / 2 + 1
    let fft = MeasurementFFT(length: n)

    var padded = [Double](repeating: 0, count: n)
    padded.replaceSubrange(0..<ir.samples.count, with: ir.samples)

    var re = [Double](repeating: 0, count: bins)
    var im = [Double](repeating: 0, count: bins)
    fft.forward(realIn: padded, specRe: &re, specIm: &im)
    return FrequencyResponse(real: re, imag: im, sampleRate: ir.sampleRate, fftSize: n)
  }

  /// Compute a `FrequencyResponse` from an `ImpulseResponse` using a
  /// Frequency-Dependent Window (FDW).
  ///
  /// Replaces the single fixed window with a Hann window whose length
  /// varies inversely with frequency: wider (more cycles) at low
  /// frequencies where modes need long observation windows, tighter at
  /// high frequencies where reflections arrive within milliseconds.
  /// Centred on the impulse peak (`ir.zeroIndex`).
  public static func fdw(
    impulseResponse ir: ImpulseResponse,
    cycles: Double,
    fftSize: Int? = nil
  ) -> FrequencyResponse {
    let n = max(2, fftSize ?? (ir.count + (ir.count % 2)))
    precondition(
      n % 2 == 0 && n >= ir.count, "FrequencyResponse: fftSize must be even and ≥ ir.count")
    let bins = n / 2 + 1

    var re = [Double](repeating: 0, count: bins)
    var im = [Double](repeating: 0, count: bins)

    let twoPi = 2.0 * Double.pi
    let p = ir.zeroIndex
    let count = ir.samples.count

    for k in 0..<bins {
      // For DC (k=0), use the window width of bin 1.
      let kEff = max(1, k)
      // Total window width in samples: W_k = cycles * N / kEff
      let w_k = cycles * Double(n) / Double(kEff)
      let h_k = w_k / 2.0

      let startIdx = max(0, Int(floor(Double(p) - h_k)))
      let endIdx = min(count - 1, Int(ceil(Double(p) + h_k)))

      var rSum: Double = 0
      var iSum: Double = 0

      let kOverN = Double(k) / Double(n)
      for i in startIdx...endIdx {
        let d = abs(Double(i - p))
        if d <= h_k {
          let w = 0.5 * (1.0 + cos(Double.pi * d / h_k))
          let angle = twoPi * kOverN * Double(i)
          rSum += ir.samples[i] * w * cos(angle)
          iSum -= ir.samples[i] * w * sin(angle)
        }
      }
      re[k] = rSum
      im[k] = iSum
    }

    return FrequencyResponse(real: re, imag: im, sampleRate: ir.sampleRate, fftSize: n)
  }

  /// Computes the Short-Time Fourier Transform (STFT) of an `ImpulseResponse`
  /// using a sliding Hann window. Produces overlapping slices for waterfall / CSD visualization.
  ///
  /// - Parameters:
  ///   - impulseResponse: The input time-domain response. Slicing starts at `zeroIndex`.
  ///   - sliceCount: Number of overlapping time slices to produce (e.g. 30).
  ///   - maxTimeSeconds: The time duration covered by the slices (e.g. 0.5 seconds).
  ///   - windowLength: Length of the sliding Hann window in samples (e.g. 2048 or 4096).
  ///   - fftSize: The FFT length for each slice (zero-padded if larger than windowLength). Must be even.
  /// - Returns: An array of `FrequencyResponse` slices, along with the relative time in seconds for each slice.
  public static func stft(
    impulseResponse ir: ImpulseResponse,
    sliceCount: Int = 30,
    maxTimeSeconds: Double = 0.5,
    windowLength: Int = 2048,
    fftSize: Int = 4096
  ) -> [(time: Double, response: FrequencyResponse)] {
    guard sliceCount > 0, windowLength > 0, fftSize % 2 == 0, fftSize > 0 else { return [] }

    let p = ir.zeroIndex
    let totalSamples = ir.samples.count
    let maxSampleOffset = Int(maxTimeSeconds * Double(ir.sampleRate))
    let timeStride = maxSampleOffset / max(1, sliceCount - 1)

    // Precompute Hann window
    var hann = [Double](repeating: 0, count: windowLength)
    for i in 0..<windowLength {
      hann[i] = 0.5 * (1.0 - cos(2.0 * Double.pi * Double(i) / Double(windowLength - 1)))
    }

    let fft = MeasurementFFT(length: fftSize)
    let bins = fftSize / 2 + 1

    var padded = [Double](repeating: 0, count: fftSize)

    var slices: [(time: Double, response: FrequencyResponse)] = []
    slices.reserveCapacity(sliceCount)

    for sliceIdx in 0..<sliceCount {
      let sampleOffset = sliceIdx * timeStride
      let t = Double(sampleOffset) / Double(ir.sampleRate)
      let sliceStart = p + sampleOffset

      for i in 0..<fftSize { padded[i] = 0.0 }

      // Apply window to the extracted block
      for wIdx in 0..<windowLength {
        let srcIdx = sliceStart + wIdx
        if srcIdx >= 0 && srcIdx < totalSamples {
          padded[wIdx] = ir.samples[srcIdx] * hann[wIdx]
        }
      }

      var re = [Double](repeating: 0, count: bins)
      var im = [Double](repeating: 0, count: bins)
      fft.forward(realIn: padded, specRe: &re, specIm: &im)

      let fr = FrequencyResponse(real: re, imag: im, sampleRate: ir.sampleRate, fftSize: fftSize)
      slices.append((time: t, response: fr))
    }

    return slices
  }
}
