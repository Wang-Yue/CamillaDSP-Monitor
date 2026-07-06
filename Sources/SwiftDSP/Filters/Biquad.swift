import Accelerate
import DSPConfig
import Foundation

extension BiquadParameters {
  /// Validate parameter ranges for the configured sample rate. Lives
  /// here (rather than next to the parameter struct) because
  /// the parameter struct must not depend on `ConfigError`.
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

  public func processSingle(_ sample: Double) -> Double {
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
