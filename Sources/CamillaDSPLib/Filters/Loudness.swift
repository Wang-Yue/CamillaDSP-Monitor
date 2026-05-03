// CamillaDSP-Swift: Loudness filter - Fletcher-Munson compensation
// Faithful translation of the Rust CamillaDSP loudness filter.
// Uses two shelving filters (70 Hz low, 3500 Hz high) with 12 dB/oct slope,
// scaled by volume relative to reference level over a 20 dB range.

import Accelerate
import Foundation

public final class LoudnessFilter: Filter {
  public let name: String
  private var referenceLevel: PrcFmt
  private var highBoost: PrcFmt
  private var lowBoost: PrcFmt
  private var attenuateMid: Bool
  private let sampleRate: Int

  private var lowShelf: BiquadFilter
  private var highShelf: BiquadFilter
  private var currentVolume: PrcFmt = 0.0
  private var active: Bool = false
  private var midGain: PrcFmt = 0.0  // for attenuate_mid mode

  public var processingParameters: ProcessingParameters?

  public init(name: String, config: FilterConfig, sampleRate: Int) {
    self.name = name
    self.sampleRate = sampleRate

    let params = config.parameters
    self.referenceLevel = params.referenceLevel ?? -25.0
    self.highBoost = params.highBoost ?? 10.0  // Rust default: 10
    self.lowBoost = params.lowBoost ?? 10.0  // Rust default: 10
    self.attenuateMid = params.attenuateMid ?? false

    let passCoeffs = BiquadCoefficients.passthrough
    self.lowShelf = BiquadFilter(
      name: "\(name)_ls", coefficients: passCoeffs, sampleRate: sampleRate)
    self.highShelf = BiquadFilter(
      name: "\(name)_hs", coefficients: passCoeffs, sampleRate: sampleRate)
  }

  /// Compute relative boost factor: 0.0 at reference level, 1.0 at 20 dB below
  private static func relBoost(level: PrcFmt, reference: PrcFmt) -> PrcFmt {
    let boost = (reference - level) / 20.0
    return max(0.0, min(1.0, boost))
  }

  public func process(waveform: inout [PrcFmt]) throws {
    guard let params = processingParameters else { return }

    let vol = params.currentVolume

    // Only recompute coefficients when volume actually changes
    if abs(vol - currentVolume) > 0.01 || !active {
      currentVolume = vol
      updateShelves(volume: vol)
    }

    guard active else { return }

    // Order matches Rust: highShelf → lowShelf → midGain
    try highShelf.process(waveform: &waveform)
    try lowShelf.process(waveform: &waveform)

    if attenuateMid && abs(midGain) > 0.001 {
      let linGain = PrcFmt.fromDB(midGain)
      waveform.withUnsafeMutableBufferPointer { ptr in
        vDSP.multiply(linGain, ptr, result: &ptr)
      }
    }
  }

  private func updateShelves(volume: PrcFmt) {
    let boost = Self.relBoost(level: volume, reference: referenceLevel)
    active = boost > 0.001  // match Rust: skip processing when boost negligible

    let lowGain = lowBoost * boost
    let highGain = highBoost * boost

    if attenuateMid {
      midGain = -max(lowGain, highGain)
    }

    // Low shelf at 70 Hz, 12 dB/oct slope
    // Update coefficients in-place to preserve biquad delay-line state (no clicks)
    var lp = FilterParameters()
    lp.freq = 70.0
    lp.gain = lowGain
    lp.slope = 12.0
    lp.subtype = BiquadType.lowshelf.rawValue
    lowShelf.updateParameters(FilterConfig(type: .biquad, parameters: lp))

    // High shelf at 3500 Hz, 12 dB/oct slope
    var hp = FilterParameters()
    hp.freq = 3500.0
    hp.gain = highGain
    hp.slope = 12.0
    hp.subtype = BiquadType.highshelf.rawValue
    highShelf.updateParameters(FilterConfig(type: .biquad, parameters: hp))
  }

  public func updateParameters(_ config: FilterConfig) {
    let params = config.parameters
    referenceLevel = params.referenceLevel ?? -25.0
    highBoost = params.highBoost ?? 10.0
    lowBoost = params.lowBoost ?? 10.0
    attenuateMid = params.attenuateMid ?? false
    // Immediately recompute shelves at current volume (preserves biquad state)
    if let processingParams = processingParameters {
      let vol = processingParams.currentVolume
      currentVolume = vol
      updateShelves(volume: vol)
      active = true
    } else {
      active = false
    }
  }
}
