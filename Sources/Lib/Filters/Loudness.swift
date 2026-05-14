// RME ADI-2 DAC Loudness Curves
// https://www.rme-audio.de/downloads/adi2dac_e.pdf

import Accelerate
import DSPAudio
import DSPConfig
import Foundation

final class LoudnessFilter: Filter {
  private let sampleRate: Int
  private var params: LoudnessParameters

  private let lowShelfFilter: BiquadFilter
  private let highShelfFilter: BiquadFilter

  private var lastVolume: PrcFmt = 0.0
  private var isProcessingActive: Bool = false
  private var midbandAttenuationDb: PrcFmt = 0.0

  var processingParameters: ProcessingParameters?

  init(parameters: LoudnessParameters, sampleRate: Int) {
    self.sampleRate = sampleRate
    self.params = parameters

    let passCoeffs = BiquadCoefficients.passthrough
    self.lowShelfFilter = BiquadFilter(coefficients: passCoeffs)
    self.highShelfFilter = BiquadFilter(coefficients: passCoeffs)
  }

  func process(waveform: MutableWaveform) {
    guard let procParams = processingParameters else { return }

    let currentVol = procParams.currentVolume

    // Recompute coefficients if volume changed significantly
    if abs(currentVol - lastVolume) > 0.01 || !isProcessingActive {
      lastVolume = currentVol
      recomputeShelves(volume: currentVol)
    }

    guard isProcessingActive else { return }

    // Apply filters in order
    highShelfFilter.process(waveform: waveform)
    lowShelfFilter.process(waveform: waveform)

    // Apply midband attenuation if enabled
    if params.attenuateMid == true && abs(midbandAttenuationDb) > 0.001 {
      let factor = PrcFmt.fromDB(midbandAttenuationDb)
      DSPOps.scalarMultiply(waveform, by: factor)
    }
  }

  private func recomputeShelves(volume: PrcFmt) {
    let ref = params.referenceLevel ?? -25.0
    let boostFactor = max(0.0, min(1.0, (ref - volume) / 20.0))

    isProcessingActive = boostFactor > 0.001

    let lowBoost = (params.lowBoost ?? 10.0) * boostFactor
    let highBoost = (params.highBoost ?? 10.0) * boostFactor

    if params.attenuateMid == true {
      midbandAttenuationDb = -max(lowBoost, highBoost)
    }
    // Low shelf at 70 Hz, 12 dB/oct slope
    // Update coefficients in-place to preserve biquad delay-line state (no clicks)
    let lpParams = BiquadParameters(type: .lowshelf, freq: 70.0, gain: lowBoost, slope: 12.0)
    lowShelfFilter.updateParameters(.biquad(lpParams), sampleRate: sampleRate)

    // High shelf at 3500 Hz, 12 dB/oct slope
    let hpParams = BiquadParameters(type: .highshelf, freq: 3500.0, gain: highBoost, slope: 12.0)
    highShelfFilter.updateParameters(.biquad(hpParams), sampleRate: sampleRate)
  }
}
