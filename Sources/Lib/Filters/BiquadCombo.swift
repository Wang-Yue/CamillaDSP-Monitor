import DSPAudio
import DSPConfig
import Foundation

final class BiquadComboFilter: Filter {
  let name: String
  private var sections: [BiquadFilter]

  init(name: String = "biquadcombo", parameters: BiquadComboParameters, sampleRate: Int) throws {
    self.name = name
    self.sections = try Self.buildSections(params: parameters, sampleRate: sampleRate)
  }

  func process(waveform: MutableWaveform) {
    for section in sections {
      section.process(waveform: waveform)
    }
  }

  func updateParameters(_ config: FilterConfig, sampleRate: Int) {
    guard case .biquadCombo(let params) = config else { return }
    if let newSections = try? Self.buildSections(params: params, sampleRate: sampleRate) {
      self.sections = newSections
    }
  }

  private static func buildSections(params: BiquadComboParameters, sampleRate: Int) throws
    -> [BiquadFilter]
  {
    let freq = params.freq ?? 1000.0
    let order = params.order ?? 4

    switch params.type {
    case .butterworthLowpass:
      return try butterworthSections(
        freq: freq, order: order, sampleRate: sampleRate, highpass: false)

    case .butterworthHighpass:
      return try butterworthSections(
        freq: freq, order: order, sampleRate: sampleRate, highpass: true)

    case .linkwitzRileyLowpass:
      let qValues = linkwitzRileyQ(order: order)
      return try makeSectionsFromQ(
        freq: freq, qValues: qValues, sampleRate: sampleRate, highpass: false)

    case .linkwitzRileyHighpass:
      let qValues = linkwitzRileyQ(order: order)
      return try makeSectionsFromQ(
        freq: freq, qValues: qValues, sampleRate: sampleRate, highpass: true)

    case .tilt:
      let slope = params.gain ?? 0.0
      return try buildTiltEQ(slope: slope, sampleRate: sampleRate)

    case .graphicEqualizer:
      let gains = params.gains ?? []
      let freqMin = params.freqMin ?? 20.0
      let freqMax = params.freqMax ?? 20000.0
      return try buildGraphicEQ(
        gains: gains, freqMin: freqMin, freqMax: freqMax, sampleRate: sampleRate)

    case .fivePointPeq:
      return try buildFivePointPEQ(params: params, sampleRate: sampleRate)
    }
  }

  // MARK: - Butterworth & Linkwitz-Riley helper calculations

  static func butterworthQ(order: Int) -> [PrcFmt] {
    var qValues: [PrcFmt] = []
    for k in 0..<(order / 2) {
      let angle = PrcFmt.pi / PrcFmt(order) * (PrcFmt(k) + 0.5)
      qValues.append(1.0 / (2.0 * sin(angle)))
    }
    if order % 2 == 1 {
      qValues.append(-1.0)
    }
    return qValues
  }

  static func linkwitzRileyQ(order: Int) -> [PrcFmt] {
    var qTemp = butterworthQ(order: order / 2)
    if order % 4 > 0 {
      qTemp.removeLast()
      var qValues = qTemp
      qValues.append(contentsOf: qTemp)
      qValues.append(0.5)
      return qValues
    } else {
      var qValues = qTemp
      qValues.append(contentsOf: qTemp)
      return qValues
    }
  }

  private static func makeSectionsFromQ(
    freq: PrcFmt, qValues: [PrcFmt], sampleRate: Int, highpass: Bool
  ) throws -> [BiquadFilter] {
    var sections: [BiquadFilter] = []
    for q in qValues {
      var p = BiquadParameters()
      p.freq = freq
      if q >= 0 {
        p.q = q
        p.type = highpass ? .highpass : .lowpass
      } else {
        p.type = highpass ? .highpassFO : .lowpassFO
      }
      let coeffs = try BiquadFilter.computeCoefficients(p, sampleRate: sampleRate)
      sections.append(BiquadFilter(coefficients: coeffs))
    }
    return sections
  }

  private static func butterworthSections(
    freq: PrcFmt, order: Int, sampleRate: Int, highpass: Bool
  ) throws -> [BiquadFilter] {
    var sections: [BiquadFilter] = []
    let n = order
    let numSOS = n / 2
    for k in 0..<numSOS {
      let angle = PrcFmt.pi / PrcFmt(n) * (PrcFmt(k) + 0.5)
      let q = 1.0 / (2.0 * sin(angle))

      var p = BiquadParameters()
      p.freq = freq
      p.q = q
      p.type = highpass ? .highpass : .lowpass

      let coeffs = try BiquadFilter.computeCoefficients(p, sampleRate: sampleRate)
      sections.append(BiquadFilter(coefficients: coeffs))
    }

    if n % 2 == 1 {
      var p = BiquadParameters()
      p.freq = freq
      p.type = highpass ? .highpassFO : .lowpassFO

      let coeffs = try BiquadFilter.computeCoefficients(p, sampleRate: sampleRate)
      sections.append(BiquadFilter(coefficients: coeffs))
    }

    return sections
  }

  // MARK: - Tilt EQ

  private static func buildTiltEQ(
    slope: PrcFmt, sampleRate: Int
  ) throws -> [BiquadFilter] {
    let gainLow = -slope / 2.0
    let gainHigh = slope / 2.0

    var lsParams = BiquadParameters()
    lsParams.freq = 110.0
    lsParams.gain = gainLow
    lsParams.q = 0.35
    lsParams.type = .lowshelf

    var hsParams = BiquadParameters()
    hsParams.freq = 3500.0
    hsParams.gain = gainHigh
    hsParams.q = 0.35
    hsParams.type = .highshelf

    let lsCoeffs = try BiquadFilter.computeCoefficients(lsParams, sampleRate: sampleRate)
    let hsCoeffs = try BiquadFilter.computeCoefficients(hsParams, sampleRate: sampleRate)

    return [
      BiquadFilter(coefficients: lsCoeffs),
      BiquadFilter(coefficients: hsCoeffs),
    ]
  }

  // MARK: - Graphic EQ

  private static func buildGraphicEQ(
    gains: [Double], freqMin: Double, freqMax: Double, sampleRate: Int
  ) throws -> [BiquadFilter] {
    let nbands = gains.count
    guard nbands > 0 else { return [] }

    let logMin = log2(freqMin)
    let logMax = log2(freqMax)
    let bw = (logMax - logMin) / PrcFmt(nbands)

    var sections: [BiquadFilter] = []
    for i in 0..<nbands {
      let g = gains[i]
      if abs(g) <= 0.001 { continue }
      let logFreq = logMin + (PrcFmt(i) + 0.5) * bw
      let freq = pow(2.0, logFreq)

      var p = BiquadParameters()
      p.freq = freq
      p.gain = g
      p.bandwidth = bw
      p.type = .peaking

      let coeffs = try BiquadFilter.computeCoefficients(p, sampleRate: sampleRate)
      sections.append(BiquadFilter(coefficients: coeffs))
    }
    return sections
  }

  // MARK: - Five Point PEQ

  private static func buildFivePointPEQ(
    params: BiquadComboParameters, sampleRate: Int
  ) throws -> [BiquadFilter] {
    var sections: [BiquadFilter] = []

    // Low shelf
    if let gLow = params.gls, abs(gLow) > 0.01, let fLow = params.fls, let qLow = params.qls {
      var p = BiquadParameters()
      p.freq = fLow
      p.gain = gLow
      p.q = qLow
      p.type = .lowshelf
      sections.append(
        BiquadFilter(coefficients: try BiquadFilter.computeCoefficients(p, sampleRate: sampleRate)))
    }

    // Mid bands
    let mids: [(f: Double?, g: Double?, q: Double?)] = [
      (params.fp1, params.gp1, params.qp1),
      (params.fp2, params.gp2, params.qp2),
      (params.fp3, params.gp3, params.qp3),
    ]
    for mid in mids {
      if let g = mid.g, abs(g) > 0.01, let f = mid.f, let q = mid.q {
        var p = BiquadParameters()
        p.freq = f
        p.gain = g
        p.q = q
        p.type = .peaking
        sections.append(
          BiquadFilter(
            coefficients: try BiquadFilter.computeCoefficients(p, sampleRate: sampleRate)))
      }
    }

    // High shelf
    if let gHigh = params.ghs, abs(gHigh) > 0.01, let fHigh = params.fhs, let qHigh = params.qhs {
      var p = BiquadParameters()
      p.freq = fHigh
      p.gain = gHigh
      p.q = qHigh
      p.type = .highshelf
      sections.append(
        BiquadFilter(coefficients: try BiquadFilter.computeCoefficients(p, sampleRate: sampleRate)))
    }

    return sections
  }
}
