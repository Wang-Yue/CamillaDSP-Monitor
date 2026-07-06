import Accelerate
import DSPConfig
import Foundation

final class BiquadFilter: Filter {
  let name: String
  private var setup: vDSP_biquadm_SetupD?

  init(name: String = "biquad", coefficients: BiquadCoefficients) {
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

  func process(waveform: MutableWaveform) {
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

  func processSingle(_ sample: Double) -> Double {
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

  func updateParameters(_ config: FilterConfig, sampleRate: Int) {
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
  static func computeCoefficients(_ params: BiquadParameters, sampleRate: Int) throws
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
