import DSPAudio
import DSPConfig
import Foundation

extension BiquadParameters {
  /// Validate parameter ranges for the configured sample rate. Lives
  /// here (rather than next to the parameter struct) because
  /// `BiquadCoefficients.swift` is shared with the Rust-FFI build and
  /// must not depend on `ConfigError`.
  public func validate(sampleRate: Int) throws {
    guard type != nil else {
      throw ConfigError.invalidFilter("Biquad filter missing 'type'")
    }

    let nyquist = Double(sampleRate) / 2.0

    if let freq = freq {
      try Self.checkFreq(freq, nyquist: nyquist, label: "freq")
    }
    if let q = q {
      try Self.checkPositive(q, label: "Q")
    }
    if let slope = slope {
      try Self.checkPositive(slope, label: "slope")
      guard slope <= 12.0 else {
        throw ConfigError.invalidFilter("slope must be <= 12.0 dB/oct, got \(slope)")
      }
    }
    if let bw = bandwidth {
      try Self.checkPositive(bw, label: "bandwidth")
    }

    // Stability check: pole positions of the realised coefficients must
    // lie strictly inside the unit circle.
    if let coeffs = BiquadCoefficients.compute(parameters: self, sampleRate: sampleRate) {
      if abs(coeffs.a2) >= 1.0 || abs(coeffs.a1) >= 1.0 + coeffs.a2 {
        throw ConfigError.invalidFilter("Unstable biquad filter specified")
      }
    }
  }

  private static func checkFreq(_ freq: Double, nyquist: Double, label: String) throws {
    guard freq > 0 else {
      throw ConfigError.invalidFilter("\(label) must be > 0, got \(freq)")
    }
    guard freq < nyquist else {
      throw ConfigError.invalidFilter("\(label) must be < Nyquist (\(nyquist) Hz), got \(freq)")
    }
  }

  private static func checkPositive(_ value: Double, label: String) throws {
    guard value > 0 else {
      throw ConfigError.invalidFilter("\(label) must be > 0, got \(value)")
    }
  }
}

public final class BiquadFilter: Filter {
  private var coeffs: BiquadCoefficients

  private var w1: PrcFmt = 0
  private var w2: PrcFmt = 0

  public init(coefficients: BiquadCoefficients) {
    self.coeffs = coefficients
  }

  public func process(waveform: MutableWaveform) {
    let b0 = coeffs.b0
    let b1 = coeffs.b1
    let b2 = coeffs.b2
    let a1 = coeffs.a1
    let a2 = coeffs.a2

    var w1 = self.w1
    var w2 = self.w2

    for i in 0..<waveform.count {
      let input = waveform[i]
      let w = input - a1 * w1 - a2 * w2
      let out = b0 * w + b1 * w1 + b2 * w2

      w2 = w1
      w1 = w

      waveform[i] = out
    }

    if w1.isSubnormal { w1 = 0 }
    if w2.isSubnormal { w2 = 0 }

    self.w1 = w1
    self.w2 = w2
  }

  public func updateParameters(_ config: FilterConfig, sampleRate: Int) {
    guard case .biquad(let params) = config else { return }
    if let newCoeffs = try? BiquadFilter.computeCoefficients(
      params, sampleRate: sampleRate)
    {
      self.coeffs = newCoeffs
    }
  }
  public static func computeCoefficients(_ params: BiquadParameters, sampleRate: Int) throws
    -> BiquadCoefficients
  {
    guard params.type != nil else {
      throw ConfigError.invalidFilter("Biquad filter missing 'type'")
    }

    guard let coeffs = BiquadCoefficients.compute(parameters: params, sampleRate: sampleRate)
    else {
      throw ConfigError.invalidFilter("Failed to compute biquad coefficients")
    }
    return coeffs
  }
}

public struct BiquadCoefficients: Sendable {
  public var b0: Double
  public var b1: Double
  public var b2: Double
  public var a1: Double
  public var a2: Double

  public static let passthrough = BiquadCoefficients(b0: 1, b1: 0, b2: 0, a1: 0, a2: 0)

  public static func compute(
    parameters: BiquadParameters,
    sampleRate: Int
  ) -> BiquadCoefficients? {
    guard let type = parameters.type else { return nil }

    let fs = Double(sampleRate)
    let freq = parameters.freq ?? 1000.0
    let gain = parameters.gain ?? 0.0
    var q = parameters.q ?? 0.707

    let w0 = 2.0 * .pi * freq / fs
    let cosW0 = cos(w0)
    let sinW0 = sin(w0)
    let A = pow(10.0, gain / 40.0)

    // Compute effective Q if bandwidth or slope is present
    if let bw = parameters.bandwidth {
      q = 1.0 / (2.0 * sinh(log(2.0) / 2.0 * bw * w0 / sinW0))
    } else if let s = parameters.slope {
      let slopeS = s / 12.0
      q = 1.0 / sqrt((A + 1.0 / A) * (1.0 / slopeS - 1.0) + 2.0)
    }

    let alpha = sinW0 / (2.0 * q)

    var b0: Double
    var b1: Double
    var b2: Double
    var a0: Double
    var a1: Double
    var a2: Double

    switch type {
    case .peaking:
      b0 = 1 + alpha * A
      b1 = -2 * cosW0
      b2 = 1 - alpha * A
      a0 = 1 + alpha / A
      a1 = -2 * cosW0
      a2 = 1 - alpha / A

    case .lowshelf:
      b0 = A * ((A + 1) - (A - 1) * cosW0 + 2 * sqrt(A) * alpha)
      b1 = 2 * A * ((A - 1) - (A + 1) * cosW0)
      b2 = A * ((A + 1) - (A - 1) * cosW0 - 2 * sqrt(A) * alpha)
      a0 = (A + 1) + (A - 1) * cosW0 + 2 * sqrt(A) * alpha
      a1 = -2 * ((A - 1) + (A + 1) * cosW0)
      a2 = (A + 1) + (A - 1) * cosW0 - 2 * sqrt(A) * alpha

    case .highshelf:
      b0 = A * ((A + 1) + (A - 1) * cosW0 + 2 * sqrt(A) * alpha)
      b1 = -2 * A * ((A - 1) + (A + 1) * cosW0)
      b2 = A * ((A + 1) + (A - 1) * cosW0 - 2 * sqrt(A) * alpha)
      a0 = (A + 1) - (A - 1) * cosW0 + 2 * sqrt(A) * alpha
      a1 = 2 * ((A - 1) - (A + 1) * cosW0)
      a2 = (A + 1) - (A - 1) * cosW0 - 2 * sqrt(A) * alpha

    case .lowpass:
      b0 = (1 - cosW0) / 2
      b1 = 1 - cosW0
      b2 = (1 - cosW0) / 2
      a0 = 1 + alpha
      a1 = -2 * cosW0
      a2 = 1 - alpha

    case .highpass:
      b0 = (1 + cosW0) / 2
      b1 = -(1 + cosW0)
      b2 = (1 + cosW0) / 2
      a0 = 1 + alpha
      a1 = -2 * cosW0
      a2 = 1 - alpha

    case .notch:
      b0 = 1
      b1 = -2 * cosW0
      b2 = 1
      a0 = 1 + alpha
      a1 = -2 * cosW0
      a2 = 1 - alpha

    case .bandpass:
      b0 = alpha
      b1 = 0
      b2 = -alpha
      a0 = 1 + alpha
      a1 = -2 * cosW0
      a2 = 1 - alpha

    case .allpass:
      b0 = 1 - alpha
      b1 = -2 * cosW0
      b2 = 1 + alpha
      a0 = 1 + alpha
      a1 = -2 * cosW0
      a2 = 1 - alpha

    case .lowpassFO:
      b0 = sinW0
      b1 = sinW0
      b2 = 0.0
      a0 = sinW0 + 1.0 + cosW0
      a1 = sinW0 - 1.0 - cosW0
      a2 = 0.0

    case .highpassFO:
      b0 = 1.0 + cosW0
      b1 = -1.0 - cosW0
      b2 = 0.0
      a0 = sinW0 + 1.0 + cosW0
      a1 = sinW0 - 1.0 - cosW0
      a2 = 0.0

    case .lowshelfFO:
      b0 = A * sinW0 + 1.0 + cosW0
      b1 = A * sinW0 - 1.0 - cosW0
      b2 = 0.0
      a0 = (1.0 / A) * sinW0 + 1.0 + cosW0
      a1 = (1.0 / A) * sinW0 - 1.0 - cosW0
      a2 = 0.0

    case .highshelfFO:
      b0 = sinW0 + A + A * cosW0
      b1 = sinW0 - A - A * cosW0
      b2 = 0.0
      a0 = sinW0 + (1.0 / A) + (1.0 / A) * cosW0
      a1 = sinW0 - (1.0 / A) - (1.0 / A) * cosW0
      a2 = 0.0

    case .allpassFO:
      b0 = sinW0 - 1.0 - cosW0
      b1 = sinW0 + 1.0 + cosW0
      b2 = 0.0
      a0 = sinW0 + 1.0 + cosW0
      a1 = sinW0 - 1.0 - cosW0
      a2 = 0.0
    }

    return BiquadCoefficients(b0: b0 / a0, b1: b1 / a0, b2: b2 / a0, a1: a1 / a0, a2: a2 / a0)
  }

  /// Magnitude response in dB at frequency `f` (Hz). Uses the analytic
  /// transfer function H(z=e^{jω}) — no time-domain simulation needed.
  /// Returns 0 dB for the degenerate case where the denominator
  /// vanishes.
  public func gainDB(atFreqHz f: Double, sampleRate: Int) -> Double {
    let w = 2.0 * .pi * f / Double(sampleRate)
    let cosW = cos(w)
    let sinW = sin(w)
    let cos2W = cos(2.0 * w)
    let sin2W = sin(2.0 * w)
    let numRe = b0 + b1 * cosW + b2 * cos2W
    let numIm = -b1 * sinW - b2 * sin2W
    let denRe = 1.0 + a1 * cosW + a2 * cos2W
    let denIm = -a1 * sinW - a2 * sin2W
    let numMagSq = numRe * numRe + numIm * numIm
    let denMagSq = denRe * denRe + denIm * denIm
    return (denMagSq > 0) ? 10.0 * log10(numMagSq / denMagSq) : 0
  }

  /// Phase response in radians at frequency `f` (Hz), wrapped to
  /// `(−π, π]`. Sign convention matches `atan2(Im(H), Re(H))`.
  public func phaseRad(atFreqHz f: Double, sampleRate: Int) -> Double {
    let w = 2.0 * .pi * f / Double(sampleRate)
    let cosW = cos(w)
    let sinW = sin(w)
    let cos2W = cos(2.0 * w)
    let sin2W = sin(2.0 * w)
    let numRe = b0 + b1 * cosW + b2 * cos2W
    let numIm = -b1 * sinW - b2 * sin2W
    let denRe = 1.0 + a1 * cosW + a2 * cos2W
    let denIm = -a1 * sinW - a2 * sin2W
    let denMagSq = denRe * denRe + denIm * denIm
    if denMagSq <= 0 { return 0 }
    let hRe = (numRe * denRe + numIm * denIm) / denMagSq
    let hIm = (numIm * denRe - numRe * denIm) / denMagSq
    return atan2(hIm, hRe)
  }
}
