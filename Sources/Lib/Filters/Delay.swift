import DSPAudio
import DSPConfig
import Foundation

/// Builds the subsample biquad allpass and returns (integerDelaySamples, optionalBiquad).
/// Matches Rust `build_subsample_biquad` exactly.
private func buildSubsampleBiquad(delay: PrcFmt) -> (Int, BiquadCoefficients?) {
  if delay < 0.1 {
    return (0, nil)
  }
  if delay < 1.1 {
    let coeff = (1.0 - delay) / (1.0 + delay)
    // 1st order Thiran allpass: coeffs a1 = coeff, b0 = coeff, b1 = 1.0, b2 = 0.0, a2 = 0.0
    let coeffs = BiquadCoefficients(b0: coeff, b1: 1.0, b2: 0.0, a1: coeff, a2: 0.0)
    return (0, coeffs)
  }

  var samples = delay.rounded(.down)
  var fraction = delay - samples
  samples -= 1.0
  fraction += 1.0
  if fraction < 1.1 {
    samples -= 1.0
    fraction += 1.0
  }
  // 2nd order Thiran allpass
  let coeff1 = 2.0 * (2.0 - fraction) / (1.0 + fraction)
  let coeff2 = (2.0 - fraction) / (2.0 + fraction) * (1.0 - fraction) / (1.0 + fraction)
  let coeffs = BiquadCoefficients(b0: coeff2, b1: coeff1, b2: 1.0, a1: coeff1, a2: coeff2)
  return (Int(samples), coeffs)
}

public final class DelayFilter: Filter {
  public let name: String
  private var queue: UnsafeMutablePointer<PrcFmt>?
  private var queueCount: Int = 0
  private var readIndex: Int = 0
  private var biquad: BiquadFilter?

  public init(name: String = "delay", parameters: DelayParameters, sampleRate: Int) {
    self.name = name

    let delay = parameters.delay
    let unit = parameters.unit ?? .ms
    let subsample = parameters.subsample ?? false

    let delaySamples = Self.computeDelaySamples(delay: delay, unit: unit, sampleRate: sampleRate)
    let (integerDelay, coeffs) = Self.buildDelay(
      delaySamples: delaySamples, subsample: subsample
    )
    if integerDelay > 0 {
      let ptr = UnsafeMutablePointer<PrcFmt>.allocate(capacity: integerDelay)
      ptr.initialize(repeating: 0.0, count: integerDelay)
      self.queue = ptr
      self.queueCount = integerDelay
    } else {
      self.queue = nil
      self.queueCount = 0
    }
    self.readIndex = 0
    if let c = coeffs {
      self.biquad = BiquadFilter(coefficients: c)
    }
  }

  deinit {
    if let q = queue {
      q.deinitialize(count: queueCount)
      q.deallocate()
    }
  }

  private static func computeDelaySamples(delay: PrcFmt, unit: DelayUnit, sampleRate: Int) -> PrcFmt
  {
    switch unit {
    case .ms:
      return delay / 1000.0 * PrcFmt(sampleRate)
    case .us:
      return delay / 1_000_000.0 * PrcFmt(sampleRate)
    case .samples:
      return delay
    case .mm:
      return delay / 1000.0 * PrcFmt(sampleRate) / 343.0
    }
  }

  private static func buildDelay(
    delaySamples: PrcFmt, subsample: Bool
  ) -> (Int, BiquadCoefficients?) {
    if subsample {
      return buildSubsampleBiquad(delay: delaySamples)
    } else {
      let samples = Int(delaySamples.rounded())
      return (samples, nil)
    }
  }

  public func process(waveform: MutableWaveform) {
    if let q = queue {
      guard let wBase = waveform.baseAddress else { return }
      var ri = readIndex
      let wCount = waveform.count
      for i in 0..<wCount {
        let delayed = q[ri]
        q[ri] = wBase[i]
        wBase[i] = delayed
        ri += 1
        if ri >= queueCount { ri = 0 }
      }
      readIndex = ri
    }
    if let bq = biquad {
      bq.process(waveform: waveform)
    }
  }

  public func processSingle(_ sample: PrcFmt) -> PrcFmt {
    var out = sample
    if let q = queue {
      let delayed = q[readIndex]
      q[readIndex] = sample
      out = delayed
      readIndex += 1
      if readIndex >= queueCount { readIndex = 0 }
    }
    if let bq = biquad {
      out = bq.processSingle(out)
    }
    return out
  }

  public func updateParameters(_ config: FilterConfig, sampleRate: Int) {
    guard case .delay(let params) = config else { return }
    let delay = params.delay
    let unit = params.unit ?? .ms
    let subsample = params.subsample ?? false

    let delaySamples = Self.computeDelaySamples(delay: delay, unit: unit, sampleRate: sampleRate)
    let (integerDelay, coeffs) = Self.buildDelay(
      delaySamples: delaySamples, subsample: subsample
    )
    if let q = self.queue {
      q.deinitialize(count: queueCount)
      q.deallocate()
    }
    if integerDelay > 0 {
      let ptr = UnsafeMutablePointer<PrcFmt>.allocate(capacity: integerDelay)
      ptr.initialize(repeating: 0.0, count: integerDelay)
      self.queue = ptr
      self.queueCount = integerDelay
    } else {
      self.queue = nil
      self.queueCount = 0
    }
    self.readIndex = 0
    if let c = coeffs {
      self.biquad = BiquadFilter(coefficients: c)
    } else {
      self.biquad = nil
    }
  }

}
