// FIR design.
//
// Three routines:
//
//   - `minimumPhase(from:sampleRate:options:)` — cepstral
//     construction of a causal, real-valued IR whose magnitude
//     response matches a biquad chain. Phase is the analytic
//     minimum-phase paired with that magnitude. Equivalent to the
//     IIR EQ in the frequency domain; no excess-phase correction.
//
//   - `linearPhase(from:sampleRate:options:)` — windowed-IFFT of the
//     biquad chain's magnitude with zero phase, then circular shift
//     so the symmetry centre lands at `(N − 1) / 2`. Symmetric IR,
//     constant group delay = `N/2` samples, magnitude identical to
//     the IIR but with pre-ring.
//
//   - `fromMeasurement(measured:target:designSampleRate:options:)` —
//     designs the IR directly from the *complex* measured frequency
//     response, inverting both magnitude AND phase: H_corr(f) =
//     target(f) / measured(f). This is the only mode that corrects
//     excess phase. Output is a windowed mixed-phase IR with constant
//     group delay = `N/2` samples (same latency as linear-phase, but
//     with phase compensation built in).
//
// All routines route their FFTs through `RealFFT` and
// return real `[PrcFmt]` IRs that load directly into a
// `ConvolutionFilter` (or persist to disk as a raw `FLOAT64` stream
// for `ConvParameters(.raw, ...)`).
//
// References:
//   - Smith, J. O. "Spectral Audio Signal Processing", §10.3
//     (real cepstrum / minimum-phase decomposition).
//   - Oppenheim & Schafer "Discrete-Time Signal Processing", §13.5
//     (Hilbert relations for min-phase systems).
//   - Norcross & Bouchard, "Inverse Filtering Design Using a Minimal-
//     Phase Target Function from Regularization", AES (2006).

import Accelerate
import DSPAudio
import DSPConfig
import DSPFFT
import DSPFilters
import Foundation

public enum FIRDesign {

  public struct Options: Sendable {
    /// FFT length used during design. Power-of-2 ≥ 1024 is
    /// recommended — gives the vDSP fast path and resolves down to
    /// `sampleRate / fftSize` Hz at the bottom of the band (≈ 5.86 Hz
    /// at 48 kHz / 8192).
    public var fftSize: Int
    /// Truncate the output IR to this many leading samples. `nil`
    /// returns the full design length. Min-phase IRs decay quickly,
    /// so a 4–8k tap output is usually plenty even when designed at
    /// 16k. Linear-phase IRs are symmetric around `(N − 1) / 2`, so
    /// truncating risks asymmetry — use `fftSize` (not `nil`) with
    /// `outputLength == nil`.
    public var outputLength: Int?
    /// Floor for the magnitude before `log()` — guards against
    /// `log(0)` for nulls in the desired response. Expressed as dB
    /// below 0 dB FS. The default (`−80 dB`) clips the deepest
    /// realistic magnitude before it drives the cepstrum unstable.
    public var floorDB: PrcFmt
    /// Pre-amp applied to the entire response before design. Useful
    /// when the chain has gain peaks ≥ 0 dB and you want headroom in
    /// the IR; matches the `EQPreset.preampGain` convention.
    public var preampDB: PrcFmt

    public init(
      fftSize: Int = 8192,
      outputLength: Int? = nil,
      floorDB: PrcFmt = -80,
      preampDB: PrcFmt = -6
    ) {
      precondition(
        fftSize >= 8 && fftSize.nonzeroBitCount == 1,
        "FIRDesign: fftSize must be a power of two ≥ 8")
      self.fftSize = fftSize
      self.outputLength = outputLength
      self.floorDB = floorDB
      self.preampDB = preampDB
    }
  }

  /// Build a minimum-phase, causal real IR whose magnitude response
  /// matches the chain at every FFT bin. See file header for the
  /// algorithm.
  public static func minimumPhase(
    from biquads: [BiquadParameters],
    sampleRate: Int,
    options: Options = Options()
  ) -> [PrcFmt] {
    let n = options.fftSize
    let bins = n / 2 + 1
    let fft = RealFFT(length: n)

    // Step 1-2: build log-magnitude spectrum from the biquad chain.
    let floorLin = pow(10.0, options.floorDB / 20.0)
    let preampLn = options.preampDB / 20.0 * log(10.0)
    var logMag = [PrcFmt](repeating: 0, count: bins)
    for k in 0..<bins {
      let f = PrcFmt(k) * PrcFmt(sampleRate) / PrcFmt(n)
      var dB: PrcFmt = 0
      for p in biquads {
        guard let coeffs = BiquadCoefficients.compute(parameters: p, sampleRate: sampleRate)
        else { continue }
        dB += coeffs.gainDB(atFreqHz: f, sampleRate: sampleRate)
      }
      let lin = max(floorLin, pow(10.0, dB / 20.0))
      logMag[k] = log(lin) + preampLn
    }

    // Step 3: inverse-FFT log|H| (treated as a real, even spectrum
    // with zero imag part) → real, even cepstrum of length n.
    let cepstrum = inverseFFTRealSpectrum(re: logMag, fft: fft)

    // Step 4: causal min-phase cepstrum c_mp.
    var cMp = [PrcFmt](repeating: 0, count: n)
    cMp[0] = cepstrum[0]
    if n / 2 > 1 {
      for i in 1..<(n / 2) {
        cMp[i] = 2.0 * cepstrum[i]
      }
    }
    cMp[n / 2] = cepstrum[n / 2]
    // cMp[n/2+1 ... n-1] stays zero (negative-quefrency half).

    // Step 5: forward-FFT c_mp → complex cepstrum spectrum
    // C_mp = log|H| + j·φ_min where φ_min = −Hilbert(log|H|).
    var cMpRe = [PrcFmt](repeating: 0, count: bins)
    var cMpIm = [PrcFmt](repeating: 0, count: bins)
    forwardFFTReal(input: cMp, re: &cMpRe, im: &cMpIm, fft: fft)

    // Step 6: H_mp = exp(C_mp). Magnitude = |H|, phase = φ_min.
    var hMpRe = [PrcFmt](repeating: 0, count: bins)
    var hMpIm = [PrcFmt](repeating: 0, count: bins)
    for k in 0..<bins {
      let er = exp(cMpRe[k])
      hMpRe[k] = er * cos(cMpIm[k])
      hMpIm[k] = er * sin(cMpIm[k])
    }

    // Step 7: inverse-FFT H_mp → real, causal min-phase IR.
    var ir = inverseFFTComplexSpectrum(re: hMpRe, im: hMpIm, fft: fft)
    let outLen = options.outputLength ?? n
    if outLen < ir.count { ir.removeLast(ir.count - outLen) }
    return ir
  }

  /// Build a linear-phase real IR whose magnitude response matches
  /// the chain. The IR is symmetric around index `(n − 1) / 2`, so
  /// the group delay is constant at `n / 2` samples. Pre-ring is the
  /// well-known cost of constant group delay.
  public static func linearPhase(
    from biquads: [BiquadParameters],
    sampleRate: Int,
    options: Options = Options()
  ) -> [PrcFmt] {
    let n = options.fftSize
    let bins = n / 2 + 1
    let fft = RealFFT(length: n)

    // Build the magnitude spectrum from the chain. For linear phase,
    // we want IR even-symmetric around the centre, which corresponds
    // to spectrum phase = −π·k·(n/2)/(n/2) = −π·k. We bake that into
    // the spectrum so the inverse FFT lands the IR centred at n/2.
    let floorLin = pow(10.0, options.floorDB / 20.0)
    let preampLin = pow(10.0, options.preampDB / 20.0)
    var hRe = [PrcFmt](repeating: 0, count: bins)
    var hIm = [PrcFmt](repeating: 0, count: bins)
    let phasePerBin = -PrcFmt.pi * PrcFmt(n / 2) / PrcFmt(n / 2)  // = −π
    // Note: phase shift = e^{−jωτ} where τ = n/2 samples. At bin k,
    // ω = 2π·k/n, so phase = −2π·k·(n/2)/n = −π·k. The cos/sin pair
    // below evaluates that with a single multiply per bin.
    for k in 0..<bins {
      let f = PrcFmt(k) * PrcFmt(sampleRate) / PrcFmt(n)
      var dB: PrcFmt = 0
      for p in biquads {
        guard let coeffs = BiquadCoefficients.compute(parameters: p, sampleRate: sampleRate)
        else { continue }
        dB += coeffs.gainDB(atFreqHz: f, sampleRate: sampleRate)
      }
      let lin = max(floorLin, pow(10.0, dB / 20.0)) * preampLin
      let phase = phasePerBin * PrcFmt(k)
      hRe[k] = lin * cos(phase)
      hIm[k] = lin * sin(phase)
    }

    let ir = inverseFFTComplexSpectrum(re: hRe, im: hIm, fft: fft)
    return ir
  }

  /// Design a FIR directly from a complex measured frequency response
  /// against a target magnitude curve. Inverts both magnitude AND
  /// phase: `H_corr(f) = target(f) / measured(f)`. The resulting IR
  /// has constant group delay (≈ `N/2` samples), same latency as
  /// the linear-phase mode, but cancels excess phase as well as
  /// magnitude — the only mode useful for systems with
  /// non-minimum-phase content above the modal region.
  ///
  /// - Parameters:
  ///   - measured: complex FR from `SweepDeconvolver` →
  ///     `FrequencyResponse.from(impulseResponse:)`. Doesn't need to
  ///     be at the design rate; we resample by nearest-bin lookup.
  ///   - target: target magnitude curve. Phase target is implicitly
  ///     zero (perfect linear-phase output).
  ///   - designSampleRate: rate the resulting IR will be deployed
  ///     at. Determines the design FFT bin spacing.
  ///   - options: design FFT length, gain caps, correction band.
  public static func fromMeasurement(
    measured: FrequencyResponse,
    target: TargetCurve,
    designSampleRate: Int,
    options: MeasurementDesignOptions = MeasurementDesignOptions()
  ) -> [PrcFmt] {
    let n = options.fftSize
    let bins = n / 2 + 1
    let fft = RealFFT(length: n)

    let measuredBinHz = PrcFmt(measured.sampleRate) / PrcFmt(measured.fftSize)
    let designBinHz = PrcFmt(designSampleRate) / PrcFmt(n)
    let floorLin = pow(10.0, options.floorDB / 20.0)
    let preampLin = pow(10.0, options.preampDB / 20.0)
    let maxBoostLin = pow(10.0, options.maxBoostDB / 20.0)
    let taperOctaves: PrcFmt = 0.5
    let lowEdgeLog = log10(options.minFreqHz)
    let highEdgeLog = log10(options.maxFreqHz)
    let blend = max(0, min(1, options.phaseBlend))

    // Build correction magnitude and the desired (linear-phase)
    // angle in two parallel arrays — we'll need both raw magnitude
    // and the linear-phase target angle to drive the cepstral
    // min-phase reconstruction below.
    var corrMag = [PrcFmt](repeating: preampLin, count: bins)
    var targetAngle = [PrcFmt](repeating: 0, count: bins)

    for k in 0..<bins {
      let freq = PrcFmt(k) * designBinHz
      let delayPhase = -PrcFmt.pi * PrcFmt(k)  // linear-phase n/2-sample delay
      var corr = preampLin
      var correction: PrcFmt = 0

      if freq >= options.minFreqHz, freq <= options.maxFreqHz {
        let mBin = Int((freq / measuredBinHz).rounded())
        let mb = max(0, min(measured.bins - 1, mBin))
        let mRe = measured.real[mb]
        let mIm = measured.imag[mb]
        let mMag = (mRe * mRe + mIm * mIm).squareRoot()
        if mMag >= floorLin {
          let targetDB = target.evaluate(atFreqHz: freq)
          let targetMag = pow(10.0, targetDB / 20.0)
          var c = targetMag / mMag
          c = min(c, maxBoostLin)
          corr = c * preampLin
          correction = -atan2(mIm, mRe)

          let logF = log10(freq)
          let lowDist = (logF - lowEdgeLog) / taperOctaves
          let highDist = (highEdgeLog - logF) / taperOctaves
          let edge = min(lowDist, highDist)
          if edge < 1.0 {
            let w = 0.5 * (1.0 - cos(PrcFmt.pi * max(0, edge)))
            corr = corr * w + preampLin * (1.0 - w)
            correction = correction * w
          }
        }
      }
      corrMag[k] = corr
      targetAngle[k] = delayPhase + correction
    }

    // For phaseBlend == 1.0 (default), skip the min-phase recon —
    // the result is identical to the original linear-phase IR.
    var hRe = [PrcFmt](repeating: 0, count: bins)
    var hIm = [PrcFmt](repeating: 0, count: bins)
    if blend >= 1.0 - 1e-9 {
      for k in 0..<bins {
        hRe[k] = corrMag[k] * cos(targetAngle[k])
        hIm[k] = corrMag[k] * sin(targetAngle[k])
      }
    } else {
      // Cepstral construction of the min-phase angle. Same recipe
      // as `minimumPhase` but we keep the result in spectrum form
      // and blend the angle with the linear-phase target.
      let minPhaseAngle = computeMinimumPhaseAngle(
        magnitude: corrMag, fft: fft, floorLin: floorLin)
      for k in 0..<bins {
        // Wrap the linear-phase target into the same branch as the
        // min-phase angle before blending, otherwise the linear
        // ramp's 2π-multiples would dominate the interpolation.
        let phi =
          blend * wrappedNear(targetAngle[k], reference: minPhaseAngle[k])
          + (1.0 - blend) * minPhaseAngle[k]
        hRe[k] = corrMag[k] * cos(phi)
        hIm[k] = corrMag[k] * sin(phi)
      }
    }

    var ir = inverseFFTComplexSpectrum(re: hRe, im: hIm, fft: fft)

    // Hann window. For full linear-phase the IR is centred at n/2;
    // for pure min-phase the IR is concentrated near n=0. Mixed
    // phase falls between. Window around the actual peak so we
    // don't truncate the impulse for low-blend designs.
    let centre = blend >= 0.99 ? n / 2 : peakIndex(of: ir)
    let halfWin = min(centre, n - centre - 1)
    if halfWin > 0 {
      for i in 0..<n {
        let dist = abs(i - centre)
        if dist > halfWin {
          ir[i] = 0
        } else {
          let w = 0.5 * (1.0 + cos(PrcFmt.pi * PrcFmt(dist) / PrcFmt(halfWin)))
          ir[i] *= w
        }
      }
    }
    return ir
  }

  /// Compute the min-phase angle paired with the given magnitude,
  /// via real cepstrum: ifft(log|H|) → causal-only cepstrum → fft →
  /// imag part is the min-phase angle (Smith §10.3).
  private static func computeMinimumPhaseAngle(
    magnitude: [PrcFmt], fft: RealFFT, floorLin: PrcFmt
  ) -> [PrcFmt] {
    let bins = magnitude.count
    let n = fft.length
    let logFloor = log(floorLin)
    var logMag = [PrcFmt](repeating: 0, count: bins)
    for k in 0..<bins {
      logMag[k] = log(max(magnitude[k], floorLin))
      if !logMag[k].isFinite { logMag[k] = logFloor }
    }
    let cepstrum = inverseFFTRealSpectrum(re: logMag, fft: fft)
    var causal = [PrcFmt](repeating: 0, count: n)
    causal[0] = cepstrum[0]
    if n / 2 >= 1 {
      for i in 1..<(n / 2) {
        causal[i] = 2.0 * cepstrum[i]
      }
      causal[n / 2] = cepstrum[n / 2]
    }
    var re = [PrcFmt](repeating: 0, count: bins)
    var im = [PrcFmt](repeating: 0, count: bins)
    forwardFFTReal(input: causal, re: &re, im: &im, fft: fft)
    return im  // imag part of FFT(causal-cepstrum) = min-phase angle
  }

  /// Wrap `phi` into the (−π, π] interval centred on `reference` so
  /// blending with `reference` doesn't have to cross a 2π boundary.
  /// Used during phase-blend interpolation.
  private static func wrappedNear(_ phi: PrcFmt, reference: PrcFmt) -> PrcFmt {
    var p = phi
    while p - reference > PrcFmt.pi { p -= 2 * PrcFmt.pi }
    while p - reference < -PrcFmt.pi { p += 2 * PrcFmt.pi }
    return p
  }

  /// Index of the absolute-largest sample in `ir`. Used to centre
  /// the Hann window for mixed-phase IRs whose peak isn't at the
  /// linear-phase n/2 location.
  private static func peakIndex(of ir: [PrcFmt]) -> Int {
    var idx = 0
    var bestAbs = 0.0
    for i in 0..<ir.count {
      let v = abs(ir[i])
      if v > bestAbs {
        bestAbs = v
        idx = i
      }
    }
    return idx
  }

  /// Tuning knobs for `fromMeasurement`. Separate type so the
  /// regular `Options` doesn't carry parameters that only apply to
  /// the measurement-driven path.
  public struct MeasurementDesignOptions: Sendable {
    public var fftSize: Int
    public var floorDB: PrcFmt
    public var preampDB: PrcFmt
    /// Cap on per-frequency boost. Real measurements have nulls
    /// (modal cancellations) where `target / measured` is huge —
    /// the cap keeps the IR from chasing them with absurd boosts
    /// the speaker / amp can't deliver anyway.
    public var maxBoostDB: PrcFmt
    /// Correction band. Outside `[minFreqHz, maxFreqHz]` the IR
    /// passes through at preamp gain (no correction). The
    /// edges are cosine-tapered over half an octave.
    public var minFreqHz: PrcFmt
    public var maxFreqHz: PrcFmt
    /// Blend between minimum-phase (0.0) and linear-phase (1.0)
    /// reconstruction. `0.0` produces a causal IR with the cepstral
    /// min-phase angle paired with the desired magnitude — ~zero
    /// latency, no pre-ring, but no excess-phase correction. `1.0`
    /// produces a centred, symmetric IR with `taps/2` latency and
    /// full magnitude+phase correction. Intermediate values
    /// linearly blend the two phase responses, trading pre-ring on
    /// transients for shorter latency.
    public var phaseBlend: PrcFmt

    public init(
      fftSize: Int = 8192,
      floorDB: PrcFmt = -60,
      preampDB: PrcFmt = -6,
      maxBoostDB: PrcFmt = 12,
      minFreqHz: PrcFmt = 30,
      maxFreqHz: PrcFmt = 18_000,
      phaseBlend: PrcFmt = 1.0
    ) {
      precondition(
        fftSize >= 8 && fftSize.nonzeroBitCount == 1,
        "FIRDesign: fftSize must be a power of two ≥ 8")
      self.fftSize = fftSize
      self.floorDB = floorDB
      self.preampDB = preampDB
      self.maxBoostDB = maxBoostDB
      self.minFreqHz = minFreqHz
      self.maxFreqHz = maxFreqHz
      self.phaseBlend = phaseBlend
    }
  }

  // MARK: - FFT plumbing

  /// Forward FFT of a real time-domain buffer. Wraps
  /// `RealFFT.forward` with the safe `withUnsafe...` calls.
  private static func forwardFFTReal(
    input: [PrcFmt],
    re: inout [PrcFmt],
    im: inout [PrcFmt],
    fft: RealFFT
  ) {
    input.withUnsafeBufferPointer { src in
      re.withUnsafeMutableBufferPointer { reBuf in
        im.withUnsafeMutableBufferPointer { imBuf in
          if let s = src.baseAddress, let r = reBuf.baseAddress, let i = imBuf.baseAddress {
            fft.forward(realIn: s, specRe: r, specIm: i)
          }
        }
      }
    }
  }

  /// Inverse FFT of a *real-valued* spectrum (imag = 0 implicitly).
  /// Returns a real even-symmetric time-domain buffer of length
  /// `fft.length`. Output is normalised so `inverse(forward(x)) ≈ x`.
  private static func inverseFFTRealSpectrum(
    re: [PrcFmt],
    fft: RealFFT
  ) -> [PrcFmt] {
    let n = fft.length
    let bins = n / 2 + 1
    let zeroIm = [PrcFmt](repeating: 0, count: bins)
    return inverseFFTComplexSpectrum(re: re, im: zeroIm, fft: fft)
  }

  /// Inverse FFT of a complex spectrum, with 1/n normalisation
  /// applied so `inverse(forward(x)) ≈ x`. (RealFFT's raw
  /// inverse multiplies by `length`.)
  private static func inverseFFTComplexSpectrum(
    re: [PrcFmt],
    im: [PrcFmt],
    fft: RealFFT
  ) -> [PrcFmt] {
    let n = fft.length
    var out = [PrcFmt](repeating: 0, count: n)
    re.withUnsafeBufferPointer { reBuf in
      im.withUnsafeBufferPointer { imBuf in
        out.withUnsafeMutableBufferPointer { outBuf in
          if let r = reBuf.baseAddress, let i = imBuf.baseAddress, let o = outBuf.baseAddress {
            fft.inverse(specRe: r, specIm: i, realOut: o)
          }
        }
      }
    }
    var invN = 1.0 / PrcFmt(n)
    out.withUnsafeMutableBufferPointer { o in
      if let base = o.baseAddress {
        vDSP_vsmulD(base, 1, &invN, base, 1, vDSP_Length(n))
      }
    }
    return out
  }
}
