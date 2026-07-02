import Accelerate
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
    if let fn = freqNotch {
      try Self.checkFreq(fn, nyquist: nyquist, label: "freq_notch")
    }
    if let fp = freqPole {
      try Self.checkFreq(fp, nyquist: nyquist, label: "freq_pole")
    }
    if let fa = freqAct {
      try Self.checkFreq(fa, nyquist: nyquist, label: "freq_act")
    }
    if let ft = freqTarget {
      try Self.checkFreq(ft, nyquist: nyquist, label: "freq_target")
    }
    if let qa = qAct {
      try Self.checkPositive(qa, label: "q_act")
    }
    if let qt = qTarget {
      try Self.checkPositive(qt, label: "q_target")
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
  public let name: String
  private var setup: vDSP_biquadm_SetupD?

  public init(name: String = "biquad", coefficients: BiquadCoefficients) {
    self.name = name
    var coefficientsArray: [Double] = [
      coefficients.b0, coefficients.b1, coefficients.b2, coefficients.a1, coefficients.a2,
    ]
    self.setup = vDSP_biquadm_CreateSetupD(&coefficientsArray, 1, 1)
  }

  deinit {
    if let setup = setup {
      vDSP_biquadm_DestroySetupD(setup)
    }
  }

  public func process(waveform: MutableWaveform) {
    guard let setup = setup, let base = waveform.baseAddress else { return }

    var signalPtr = UnsafePointer(base)
    var outputPtr = base

    vDSP_biquadmD(
      setup,
      &signalPtr,
      1,
      &outputPtr,
      1,
      vDSP_Length(waveform.count)
    )
  }

  public func processSingle(_ sample: PrcFmt) -> PrcFmt {
    guard let setup = setup else { return sample }
    var inVal = sample
    var outVal = 0.0
    withUnsafePointer(to: &inVal) { inPtr in
      withUnsafeMutablePointer(to: &outVal) { outPtr in
        var signalPtr = inPtr
        var destPtr = outPtr
        vDSP_biquadmD(setup, &signalPtr, 1, &destPtr, 1, 1)
      }
    }
    return outVal
  }

  public func updateParameters(_ config: FilterConfig, sampleRate: Int) {
    guard case .biquad(let params) = config else { return }
    if let newCoeffs = try? BiquadFilter.computeCoefficients(
      params, sampleRate: sampleRate)
    {
      var coefficientsArray: [Double] = [
        newCoeffs.b0, newCoeffs.b1, newCoeffs.b2, newCoeffs.a1, newCoeffs.a2,
      ]
      if let setup = self.setup {
        vDSP_biquadm_SetCoefficientsDoubleD(setup, &coefficientsArray, 0, 0, 1, 1)
      }
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
    case .free:
      b0 = parameters.b0 ?? 1.0
      b1 = parameters.b1 ?? 0.0
      b2 = parameters.b2 ?? 0.0
      a0 = 1.0
      a1 = parameters.a1 ?? 0.0
      a2 = parameters.a2 ?? 0.0

    case .generalNotch:
      let freqZ = parameters.freqNotch ?? 1000.0
      let freqP = parameters.freqPole ?? 1000.0
      let qP = parameters.qP ?? parameters.q ?? 0.5
      let normalize = parameters.normalizeAtDc ?? true
      let tnZ = tan(.pi * freqZ / fs)
      let tnP = tan(.pi * freqP / fs)
      let alphaP = tnP / qP
      let tn2P = tnP * tnP
      let tn2Z = tnZ * tnZ
      let gainNorm = normalize ? tn2P / tn2Z : 1.0
      b0 = gainNorm * (1.0 + tn2Z)
      b1 = -2.0 * gainNorm * (1.0 - tn2Z)
      b2 = gainNorm * (1.0 + tn2Z)
      a0 = 1.0 + alphaP + tn2P
      a1 = -2.0 + 2.0 * tn2P
      a2 = 1.0 - alphaP + tn2P

    case .linkwitzTransform:
      let freqAct = parameters.freqAct ?? 50.0
      let qAct = parameters.qAct ?? 0.707
      let freqTarget = parameters.freqTarget ?? 25.0
      let qTarget = parameters.qTarget ?? 0.707
      let d0i = pow(2.0 * .pi * freqAct, 2)
      let d1i = (2.0 * .pi * freqAct) / qAct
      let c0i = pow(2.0 * .pi * freqTarget, 2)
      let c1i = (2.0 * .pi * freqTarget) / qTarget
      let fc = (freqTarget + freqAct) / 2.0
      let gn = 2.0 * .pi * fc / tan(.pi * fc / fs)
      let gn2 = gn * gn
      let cci = c0i + gn * c1i + gn2
      b0 = (d0i + gn * d1i + gn2) / cci
      b1 = 2.0 * (d0i - gn2) / cci
      b2 = (d0i - gn * d1i + gn2) / cci
      a0 = 1.0
      a1 = 2.0 * (c0i - gn2) / cci
      a2 = (c0i - gn * c1i + gn2) / cci

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
