// CamillaDSP-Swift: Filter protocol - processes a single channel's waveform

import Foundation

/// Protocol for all audio filters. Filters operate on one channel at a time.
public protocol Filter: AnyObject {
  /// Process a waveform buffer in-place
  func process(waveform: inout [PrcFmt]) throws
  /// Update filter parameters from configuration
  func updateParameters(_ config: FilterConfig)
  /// Filter name for identification
  var name: String { get }
}

/// Per-filter config validation, dispatched by filter type
public enum FilterValidator {
  public static func validate(_ config: FilterConfig, sampleRate: Int) throws {
    let fs = Double(sampleRate)
    let nyquist = fs / 2.0
    let params = config.parameters

    switch config.type {
    case .biquad:
      try validateBiquad(params, nyquist: nyquist, sampleRate: sampleRate)
    case .gain:
      try validateGain(params)
    case .volume:
      try validateVolume(params)
    case .loudness:
      try validateLoudness(params)
    }
  }

  // MARK: - Biquad

  private static func validateBiquad(_ p: FilterParameters, nyquist: Double, sampleRate: Int) throws
  {
    let subtype = BiquadType(rawValue: p.subtype ?? "Peaking") ?? .peaking

    switch subtype {
    case .free:
      break  // no freq/Q validation for free-form coefficients
    case .linkwitzTransform:
      if let f = p.freqAct { try checkFreq(f, nyquist: nyquist, label: "freq_act") }
      if let f = p.freqTarget { try checkFreq(f, nyquist: nyquist, label: "freq_target") }
      if let q = p.qAct { try checkPositive(q, label: "q_act") }
      if let q = p.qTarget { try checkPositive(q, label: "q_target") }
    case .generalNotch:
      if let f = p.freqPole { try checkFreq(f, nyquist: nyquist, label: "freq_pole") }
      if let f = p.freqNotch { try checkFreq(f, nyquist: nyquist, label: "freq_notch") }
      if let q = p.q { try checkPositive(q, label: "Q") }
    default:
      if let freq = p.freq {
        try checkFreq(freq, nyquist: nyquist, label: "freq")
      }
      if let q = p.q {
        try checkPositive(q, label: "Q")
      }
      if let slope = p.slope {
        try checkPositive(slope, label: "slope")
        guard slope <= 12.0 else {
          throw ConfigError.invalidFilter("slope must be <= 12.0 dB/oct, got \(slope)")
        }
      }
      if let bw = p.bandwidth {
        try checkPositive(bw, label: "bandwidth")
      }
    }

    // Stability check: try computing coefficients
    if let coeffs = try? BiquadFilter.computeCoefficients(p, sampleRate: sampleRate) {
      let a1 = coeffs.a1
      let a2 = coeffs.a2
      // Check poles inside unit circle
      if abs(a2) >= 1.0 || abs(a1) >= 1.0 + a2 {
        throw ConfigError.invalidFilter("Unstable biquad filter specified")
      }
    }
  }

  // MARK: - Gain

  private static func validateGain(_ p: FilterParameters) throws {
    if let gain = p.gain {
      guard gain > -150 && gain < 150 else {
        throw ConfigError.invalidFilter("gain must be in (-150, 150) dB, got \(gain)")
      }
    }
  }

  // MARK: - Volume

  private static func validateVolume(_ p: FilterParameters) throws {
    if let ramp = p.rampTime {
      guard ramp >= 0 else {
        throw ConfigError.invalidFilter("ramp_time must be >= 0, got \(ramp)")
      }
    }
  }

  // MARK: - Loudness

  private static func validateLoudness(_ p: FilterParameters) throws {
    if let ref = p.referenceLevel {
      guard ref > -100 && ref < 20 else {
        throw ConfigError.invalidFilter("reference_level must be in (-100, 20), got \(ref)")
      }
    }
    if let boost = p.highBoost {
      guard boost >= 0 && boost <= 20 else {
        throw ConfigError.invalidFilter("high_boost must be in [0, 20], got \(boost)")
      }
    }
    if let boost = p.lowBoost {
      guard boost >= 0 && boost <= 20 else {
        throw ConfigError.invalidFilter("low_boost must be in [0, 20], got \(boost)")
      }
    }
  }

  // MARK: - Helpers

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

/// Factory to create filter instances from configuration
public enum FilterFactory {
  public static func create(
    name: String,
    config: FilterConfig,
    sampleRate: Int,
    chunkSize: Int
  ) throws -> Filter {
    try FilterValidator.validate(config, sampleRate: sampleRate)
    switch config.type {
    case .gain:
      return GainFilter(name: name, config: config)
    case .volume:
      return VolumeFilter(name: name, config: config, sampleRate: sampleRate, chunkSize: chunkSize)
    case .loudness:
      return LoudnessFilter(name: name, config: config, sampleRate: sampleRate)
    case .biquad:
      return try BiquadFilter(name: name, config: config, sampleRate: sampleRate)
    }
  }
}
