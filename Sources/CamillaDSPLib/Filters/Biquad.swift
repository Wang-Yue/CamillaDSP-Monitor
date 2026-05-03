// CamillaDSP-Swift: Biquad filter - second-order IIR section
// Implements the Audio EQ Cookbook formulas with Direct Form II Transposed

import Accelerate
import Foundation

/// Biquad coefficients (normalized: a0 = 1.0)
public struct BiquadCoefficients: Sendable {
  public var b0: PrcFmt
  public var b1: PrcFmt
  public var b2: PrcFmt
  public var a1: PrcFmt
  public var a2: PrcFmt

  public static let passthrough = BiquadCoefficients(b0: 1, b1: 0, b2: 0, a1: 0, a2: 0)
}

public final class BiquadFilter: Filter {
  public let name: String
  private var coeffs: BiquadCoefficients
  private let sampleRate: Int

  /// `true` → use a scalar Direct Form II Transposed loop that matches
  /// camilladsp's Rust biquad bit-for-bit. Set globally by the env var
  /// `BIQUAD_SCALAR=1`, or per-instance via the constructor parameter.
  /// Production audio paths leave this `false` to keep the NEON-vectorised
  /// `vDSP.Biquad` fast path; bit-exact comparison tests opt in.
  private let useScalarPath: Bool
  private var biquad: vDSP.Biquad<Double>?

  // Scalar DF2T state — only meaningful when `useScalarPath == true`.
  private var s1: PrcFmt = 0
  private var s2: PrcFmt = 0

  /// Re-checked on every construction (not cached) so tests can flip the env
  /// var dynamically — important when LoudnessFilter or other consumers
  /// build biquads internally and we want them to follow the same setting.
  private static var envScalarFlag: Bool {
    ProcessInfo.processInfo.environment["BIQUAD_SCALAR"] == "1"
  }

  public init(name: String, config: FilterConfig, sampleRate: Int) throws {
    self.name = name
    self.sampleRate = sampleRate
    self.coeffs = try BiquadFilter.computeCoefficients(config.parameters, sampleRate: sampleRate)
    self.useScalarPath = BiquadFilter.envScalarFlag
    setupBiquad()
  }

  public init(name: String, coefficients: BiquadCoefficients, sampleRate: Int) {
    self.name = name
    self.sampleRate = sampleRate
    self.coeffs = coefficients
    self.useScalarPath = BiquadFilter.envScalarFlag
    setupBiquad()
  }

  /// Per-instance opt-in for the scalar path. Tests use this to compare
  /// bit-for-bit against camilladsp without affecting the rest of the suite.
  public init(
    name: String, coefficients: BiquadCoefficients, sampleRate: Int, useScalarPath: Bool
  ) {
    self.name = name
    self.sampleRate = sampleRate
    self.coeffs = coefficients
    self.useScalarPath = useScalarPath
    setupBiquad()
  }

  private func setupBiquad() {
    if useScalarPath {
      // Scalar DF2T uses `coeffs` directly each sample; no vDSP setup needed,
      // and the state vars are already 0-initialised (or carried over from the
      // previous parameter set, matching camilladsp's
      // `update_parameters` behaviour).
      self.biquad = nil
    } else {
      let c = [coeffs.b0, coeffs.b1, coeffs.b2, coeffs.a1, coeffs.a2]
      self.biquad = vDSP.Biquad(
        coefficients: c, channelCount: 1, sectionCount: 1, ofType: Double.self)
    }
  }

  public func process(waveform: inout [PrcFmt]) throws {
    if useScalarPath {
      processScalar(waveform: &waveform)
    } else {
      self.biquad?.apply(input: waveform, output: &waveform)
    }
  }

  /// Scalar Direct Form II Transposed — strictly mirrors camilladsp's
  /// `Biquad::process_single` (`filters/biquad.rs`):
  ///
  ///     out = s1 + b0 * input
  ///     s1  = s2 + b1 * input - a1 * out
  ///     s2  = b2 * input - a2 * out
  ///
  /// followed by a subnormal flush of the state — important on long
  /// silences with high-Q filters where the recursion can decay into the
  /// subnormal range and stall the FPU.
  @inline(__always)
  private func processScalar(waveform: inout [PrcFmt]) {
    let b0 = coeffs.b0
    let b1 = coeffs.b1
    let b2 = coeffs.b2
    let a1 = coeffs.a1
    let a2 = coeffs.a2
    var s1 = self.s1
    var s2 = self.s2
    waveform.withUnsafeMutableBufferPointer { ptr in
      for i in 0..<ptr.count {
        let input = ptr[i]
        let out = s1 + b0 * input
        s1 = s2 + b1 * input - a1 * out
        s2 = b2 * input - a2 * out
        ptr[i] = out
      }
    }
    if s1.isSubnormal { s1 = 0 }
    if s2.isSubnormal { s2 = 0 }
    self.s1 = s1
    self.s2 = s2
  }

  public func updateParameters(_ config: FilterConfig) {
    if let newCoeffs = try? BiquadFilter.computeCoefficients(
      config.parameters, sampleRate: sampleRate)
    {
      coeffs = newCoeffs
      setupBiquad()
    }
  }

  /// Reset filter state
  public func reset() {
    s1 = 0
    s2 = 0
    setupBiquad()
  }

  /// Compute biquad coefficients from parameters using Audio EQ Cookbook formulas
  public static func computeCoefficients(_ params: FilterParameters, sampleRate: Int) throws
    -> BiquadCoefficients
  {
    guard let biquadType = params.biquadType else {
      throw ConfigError.invalidFilter("Biquad filter missing 'type'")
    }

    let fs = PrcFmt(sampleRate)

    switch biquadType {
    case .free:
      return BiquadCoefficients(
        b0: params.b0 ?? 1.0,
        b1: params.b1 ?? 0.0,
        b2: params.b2 ?? 0.0,
        a1: params.a1 ?? 0.0,
        a2: params.a2 ?? 0.0
      )

    case .lowpass:
      let freq = params.freq ?? 1000.0
      let q = params.q ?? 0.707
      let w0 = 2.0 * .pi * freq / fs
      let alpha = sin(w0) / (2.0 * q)
      let cosw0 = cos(w0)
      let a0 = 1.0 + alpha
      return BiquadCoefficients(
        b0: ((1.0 - cosw0) / 2.0) / a0,
        b1: (1.0 - cosw0) / a0,
        b2: ((1.0 - cosw0) / 2.0) / a0,
        a1: (-2.0 * cosw0) / a0,
        a2: (1.0 - alpha) / a0
      )

    case .highpass:
      let freq = params.freq ?? 1000.0
      let q = params.q ?? 0.707
      let w0 = 2.0 * .pi * freq / fs
      let alpha = sin(w0) / (2.0 * q)
      let cosw0 = cos(w0)
      let a0 = 1.0 + alpha
      return BiquadCoefficients(
        b0: ((1.0 + cosw0) / 2.0) / a0,
        b1: (-(1.0 + cosw0)) / a0,
        b2: ((1.0 + cosw0) / 2.0) / a0,
        a1: (-2.0 * cosw0) / a0,
        a2: (1.0 - alpha) / a0
      )

    case .lowpassFO:
      let freq = params.freq ?? 1000.0
      let w0 = 2.0 * .pi * freq / fs
      let k = tan(w0 / 2.0)
      let a0 = k + 1.0
      return BiquadCoefficients(
        b0: k / a0,
        b1: k / a0,
        b2: 0.0,
        a1: (k - 1.0) / a0,
        a2: 0.0
      )

    case .highpassFO:
      let freq = params.freq ?? 1000.0
      let w0 = 2.0 * .pi * freq / fs
      let k = tan(w0 / 2.0)
      let a0 = k + 1.0
      return BiquadCoefficients(
        b0: 1.0 / a0,
        b1: -1.0 / a0,
        b2: 0.0,
        a1: (k - 1.0) / a0,
        a2: 0.0
      )

    case .peaking:
      let freq = params.freq ?? 1000.0
      let gain = params.gain ?? 0.0
      let w0 = 2.0 * .pi * freq / fs
      let sn = sin(w0)
      let a = pow(10.0, gain / 40.0)
      let alpha: PrcFmt
      if let bw = params.bandwidth {
        // Direct alpha from bandwidth (matches Rust: alpha = sn * sinh(ln2/2 * bw * w0/sn))
        alpha = sn * sinh(log(2.0) / 2.0 * bw * w0 / sn)
      } else {
        alpha = sn / (2.0 * (params.q ?? 1.0))
      }
      let cosw0 = cos(w0)
      let a0 = 1.0 + alpha / a
      return BiquadCoefficients(
        b0: (1.0 + alpha * a) / a0,
        b1: (-2.0 * cosw0) / a0,
        b2: (1.0 - alpha * a) / a0,
        a1: (-2.0 * cosw0) / a0,
        a2: (1.0 - alpha / a) / a0
      )

    case .lowshelf:
      let freq = params.freq ?? 1000.0
      let gain = params.gain ?? 0.0
      let a = pow(10.0, gain / 40.0)
      let w0 = 2.0 * .pi * freq / fs
      let sn = sin(w0)
      let alpha: PrcFmt
      if let slope = params.slope {
        // Matches Rust: alpha = sn/2 * sqrt((A + 1/A) * (1/(slope/12) - 1) + 2)
        let slopeS = slope / 12.0
        alpha = sn / 2.0 * sqrt((a + 1.0 / a) * (1.0 / slopeS - 1.0) + 2.0)
      } else {
        let q = params.q ?? 0.707
        alpha = sn / (2.0 * q)
      }
      let cosw0 = cos(w0)
      let sqrtA = sqrt(a)
      let a0 = (a + 1.0) + (a - 1.0) * cosw0 + 2.0 * sqrtA * alpha
      return BiquadCoefficients(
        b0: (a * ((a + 1.0) - (a - 1.0) * cosw0 + 2.0 * sqrtA * alpha)) / a0,
        b1: (2.0 * a * ((a - 1.0) - (a + 1.0) * cosw0)) / a0,
        b2: (a * ((a + 1.0) - (a - 1.0) * cosw0 - 2.0 * sqrtA * alpha)) / a0,
        a1: (-2.0 * ((a - 1.0) + (a + 1.0) * cosw0)) / a0,
        a2: ((a + 1.0) + (a - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0
      )

    case .highshelf:
      let freq = params.freq ?? 1000.0
      let gain = params.gain ?? 0.0
      let a = pow(10.0, gain / 40.0)
      let w0 = 2.0 * .pi * freq / fs
      let sn = sin(w0)
      let alpha: PrcFmt
      if let slope = params.slope {
        let slopeS = slope / 12.0
        alpha = sn / 2.0 * sqrt((a + 1.0 / a) * (1.0 / slopeS - 1.0) + 2.0)
      } else {
        let q = params.q ?? 0.707
        alpha = sn / (2.0 * q)
      }
      let cosw0 = cos(w0)
      let sqrtA = sqrt(a)
      let a0 = (a + 1.0) - (a - 1.0) * cosw0 + 2.0 * sqrtA * alpha
      return BiquadCoefficients(
        b0: (a * ((a + 1.0) + (a - 1.0) * cosw0 + 2.0 * sqrtA * alpha)) / a0,
        b1: (-2.0 * a * ((a - 1.0) + (a + 1.0) * cosw0)) / a0,
        b2: (a * ((a + 1.0) + (a - 1.0) * cosw0 - 2.0 * sqrtA * alpha)) / a0,
        a1: (2.0 * ((a - 1.0) - (a + 1.0) * cosw0)) / a0,
        a2: ((a + 1.0) - (a - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0
      )

    case .lowshelfFO:
      // Matches Rust: ampl = 10^(gain/40), bilinear with tn = tan(w/2)
      let freq = params.freq ?? 1000.0
      let gain = params.gain ?? 0.0
      let omega = 2.0 * .pi * freq / fs
      let tn = tan(omega / 2.0)
      let ampl = pow(10.0, gain / 40.0)
      let b0 = ampl * ampl * tn + ampl
      let b1 = ampl * ampl * tn - ampl
      let a0 = tn + ampl
      let a1 = tn - ampl
      return BiquadCoefficients(b0: b0 / a0, b1: b1 / a0, b2: 0, a1: a1 / a0, a2: 0)

    case .highshelfFO:
      // Matches Rust: ampl = 10^(gain/40), bilinear with tn = tan(w/2)
      let freq = params.freq ?? 1000.0
      let gain = params.gain ?? 0.0
      let omega = 2.0 * .pi * freq / fs
      let tn = tan(omega / 2.0)
      let ampl = pow(10.0, gain / 40.0)
      let b0 = ampl * tn + ampl * ampl
      let b1 = ampl * tn - ampl * ampl
      let a0 = ampl * tn + 1.0
      let a1 = ampl * tn - 1.0
      return BiquadCoefficients(b0: b0 / a0, b1: b1 / a0, b2: 0, a1: a1 / a0, a2: 0)

    case .notch:
      let freq = params.freq ?? 1000.0
      let w0 = 2.0 * .pi * freq / fs
      let sn = sin(w0)
      let cs = cos(w0)
      let alpha: PrcFmt
      if let bw = params.bandwidth {
        alpha = sn * sinh(log(2.0) / 2.0 * bw * w0 / sn)
      } else {
        alpha = sn / (2.0 * (params.q ?? 1.0))
      }
      let a0 = 1.0 + alpha
      return BiquadCoefficients(
        b0: 1.0 / a0,
        b1: (-2.0 * cs) / a0,
        b2: 1.0 / a0,
        a1: (-2.0 * cs) / a0,
        a2: (1.0 - alpha) / a0
      )

    case .generalNotch:
      // Matches Rust: bilinear transform with tan(pi*f/fs) pre-warping
      let freqZ = params.freqNotch ?? 1000.0
      let freqP = params.freqPole ?? 1000.0
      let qP = params.q ?? 0.5
      let normalize = params.normalizeAtDc ?? true
      let tnZ = tan(.pi * freqZ / fs)
      let tnP = tan(.pi * freqP / fs)
      let alphaP = tnP / qP
      let tn2P = tnP * tnP
      let tn2Z = tnZ * tnZ
      let gainNorm = normalize ? tn2P / tn2Z : 1.0
      let b0 = gainNorm * (1.0 + tn2Z)
      let b1 = -2.0 * gainNorm * (1.0 - tn2Z)
      let b2 = gainNorm * (1.0 + tn2Z)
      let a0 = 1.0 + alphaP + tn2P
      let a1 = -2.0 + 2.0 * tn2P
      let a2 = 1.0 - alphaP + tn2P
      return BiquadCoefficients(b0: b0 / a0, b1: b1 / a0, b2: b2 / a0, a1: a1 / a0, a2: a2 / a0)

    case .bandpass:
      let freq = params.freq ?? 1000.0
      let w0 = 2.0 * .pi * freq / fs
      let sn = sin(w0)
      let cs = cos(w0)
      let alpha: PrcFmt
      if let bw = params.bandwidth {
        alpha = sn * sinh(log(2.0) / 2.0 * bw * w0 / sn)
      } else {
        alpha = sn / (2.0 * (params.q ?? 1.0))
      }
      let a0 = 1.0 + alpha
      return BiquadCoefficients(
        b0: alpha / a0,
        b1: 0.0,
        b2: -alpha / a0,
        a1: (-2.0 * cs) / a0,
        a2: (1.0 - alpha) / a0
      )

    case .allpass:
      let freq = params.freq ?? 1000.0
      let w0 = 2.0 * .pi * freq / fs
      let sn = sin(w0)
      let cs = cos(w0)
      let alpha: PrcFmt
      if let bw = params.bandwidth {
        alpha = sn * sinh(log(2.0) / 2.0 * bw * w0 / sn)
      } else {
        alpha = sn / (2.0 * (params.q ?? 0.707))
      }
      let a0 = 1.0 + alpha
      return BiquadCoefficients(
        b0: (1.0 - alpha) / a0,
        b1: (-2.0 * cs) / a0,
        b2: (1.0 + alpha) / a0,
        a1: (-2.0 * cs) / a0,
        a2: (1.0 - alpha) / a0
      )

    case .allpassFO:
      let freq = params.freq ?? 1000.0
      let w0 = 2.0 * .pi * freq / fs
      let k = tan(w0 / 2.0)
      let a0 = k + 1.0
      return BiquadCoefficients(
        b0: (k - 1.0) / a0,
        b1: 1.0,
        b2: 0.0,
        a1: (k - 1.0) / a0,
        a2: 0.0
      )

    case .linkwitzTransform:
      // Matches Rust: analog prototype with bilinear pre-warping at midpoint frequency
      let freqAct = params.freqAct ?? 50.0
      let qAct = params.qAct ?? 0.707
      let freqTarget = params.freqTarget ?? 25.0
      let qTarget = params.qTarget ?? 0.707
      let d0i = pow(2.0 * .pi * freqAct, 2)
      let d1i = (2.0 * .pi * freqAct) / qAct
      let c0i = pow(2.0 * .pi * freqTarget, 2)
      let c1i = (2.0 * .pi * freqTarget) / qTarget
      let fc = (freqTarget + freqAct) / 2.0
      let gn = 2.0 * .pi * fc / tan(.pi * fc / fs)
      let gn2 = gn * gn
      let cci = c0i + gn * c1i + gn2
      let b0 = (d0i + gn * d1i + gn2) / cci
      let b1 = 2.0 * (d0i - gn2) / cci
      let b2 = (d0i - gn * d1i + gn2) / cci
      let a1 = 2.0 * (c0i - gn2) / cci
      let a2 = (c0i - gn * c1i + gn2) / cci
      return BiquadCoefficients(b0: b0, b1: b1, b2: b2, a1: a1, a2: a2)
    }
  }
}
