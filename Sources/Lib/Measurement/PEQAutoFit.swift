// Parametric-EQ auto-fit by coordinate-descent local optimization.
//
// Three phases:
//
//   Phase 1 — Seed. Place candidate shelves at the band edges
//             (low/high) and greedy peakings on the residual peaks.
//             This gives the optimizer a good starting point —
//             greedy alone already produces a reasonable fit, just
//             not a tight one.
//
//   Phase 2 — Coordinate descent. For each band, golden-section
//             search over `freq` (log-space, ±1 octave), `gain`, and
//             `Q` while the other bands are held fixed. The
//             objective is the sum of squared residuals on the log
//             grid. Cycle through all bands for K passes; stop when
//             the parameter changes settle below an epsilon.
//
//   Phase 3 — Cleanup. Drop bands whose `|gain|` falls below
//             `dropGainDB`. Refinement sometimes drives bands to
//             zero gain; carrying them around clutters the EQ
//             chain.
//
// The greedy fitter (the prior implementation here) was a single
// pass — every band saw the residual without consideration for
// later bands. With the same greedy seed across runs, the result
// looked nearly identical regardless of input variation. The
// coordinate-descent pass actually finds the LOCAL OPTIMUM per
// band given the rest of the chain, so different inputs produce
// visibly different fits.

import DSPAudio
import DSPConfig
import DSPFilters
import Foundation

public enum PEQAutoFit {

  public struct Options: Sendable {
    /// Maximum number of bands (shelves + peakings combined).
    /// Refinement may shrink the final count via cleanup.
    public var bandCount: Int
    /// Frequency grid bounds — the resampling grid and the allowable
    /// range for placed band centre frequencies.
    public var minFreqHz: PrcFmt
    public var maxFreqHz: PrcFmt
    /// Cap on per-band gain.
    public var maxGainDB: PrcFmt
    /// Q is clamped to `[minQ, maxQ]`.
    public var minQ: PrcFmt
    public var maxQ: PrcFmt
    /// Stop placing bands once `max|residual|` drops below this in
    /// the seed phase.
    public var convergenceDB: PrcFmt
    /// Whether to seed low-/high-shelf candidates.
    public var addEndpointShelves: Bool
    /// Initial corner frequencies for the candidate shelves. The
    /// optimizer is free to move them.
    public var lowShelfFreqHz: PrcFmt
    public var highShelfFreqHz: PrcFmt
    /// Coordinate-descent passes.
    public var refinementIterations: Int
    /// Bands with `|gain| < dropGainDB` are removed after refinement.
    public var dropGainDB: PrcFmt
    /// Modal-mode constraints. When enabled, peakings placed below
    /// `schroederHz` are restricted to *negative* gain (cuts only —
    /// boosting a modal null doesn't fill it, the mode is still
    /// there) and to higher Q (≥ `modalMinQ`); endpoint shelves
    /// below the Schroeder freq are suppressed too. Standard
    /// best-practice for low-frequency room correction.
    public var modalMode: Bool
    public var schroederHz: PrcFmt
    public var modalMinQ: PrcFmt
    /// Pre-smoothing width on the log-frequency grid, midband.
    /// AutoEQ defaults to 1/12 octave; that's also tight enough to
    /// preserve narrow modes while suppressing bin-level noise.
    public var smoothingOctaves: PrcFmt
    /// Smoothing width above `transitionHighHz` — the treble band is
    /// noisy and perceptually less sensitive, so AutoEQ uses 2
    /// octaves there. Sigmoid blend between `transitionLowHz` and
    /// `transitionHighHz`.
    public var trebleSmoothingOctaves: PrcFmt
    public var smoothingTransitionLow: PrcFmt
    public var smoothingTransitionHigh: PrcFmt

    public init(
      bandCount: Int = 10,
      minFreqHz: PrcFmt = 20,
      maxFreqHz: PrcFmt = 20_000,
      maxGainDB: PrcFmt = 12,
      minQ: PrcFmt = 0.3,
      maxQ: PrcFmt = 10,
      convergenceDB: PrcFmt = 0.3,
      addEndpointShelves: Bool = true,
      lowShelfFreqHz: PrcFmt = 80,
      highShelfFreqHz: PrcFmt = 8_000,
      refinementIterations: Int = 8,
      dropGainDB: PrcFmt = 0.5,
      modalMode: Bool = false,
      schroederHz: PrcFmt = 200,
      modalMinQ: PrcFmt = 2.0,
      smoothingOctaves: PrcFmt = 1.0 / 12.0,
      trebleSmoothingOctaves: PrcFmt = 2.0,
      smoothingTransitionLow: PrcFmt = 6_000,
      smoothingTransitionHigh: PrcFmt = 8_000
    ) {
      self.bandCount = bandCount
      self.minFreqHz = minFreqHz
      self.maxFreqHz = maxFreqHz
      self.maxGainDB = maxGainDB
      self.minQ = minQ
      self.maxQ = maxQ
      self.convergenceDB = convergenceDB
      self.addEndpointShelves = addEndpointShelves
      self.lowShelfFreqHz = lowShelfFreqHz
      self.highShelfFreqHz = highShelfFreqHz
      self.refinementIterations = refinementIterations
      self.dropGainDB = dropGainDB
      self.modalMode = modalMode
      self.schroederHz = schroederHz
      self.modalMinQ = modalMinQ
      self.smoothingOctaves = smoothingOctaves
      self.trebleSmoothingOctaves = trebleSmoothingOctaves
      self.smoothingTransitionLow = smoothingTransitionLow
      self.smoothingTransitionHigh = smoothingTransitionHigh
    }
  }

  // MARK: - Public helpers

  /// Build a log-spaced frequency grid over `[fMin, fMax]` with
  /// `count` points (inclusive of endpoints).
  public static func logFrequencyGrid(
    fMin: PrcFmt, fMax: PrcFmt, count: Int
  ) -> [PrcFmt] {
    precondition(count >= 2 && fMin > 0 && fMax > fMin)
    let logMin = log10(fMin)
    let logMax = log10(fMax)
    var out = [PrcFmt](repeating: 0, count: count)
    for i in 0..<count {
      let t = PrcFmt(i) / PrcFmt(count - 1)
      out[i] = pow(10, logMin + t * (logMax - logMin))
    }
    return out
  }

  /// Sample a `FrequencyResponse` onto a log-spaced grid in dB.
  public static func sampleMagnitudeDB(
    of fr: FrequencyResponse, atFrequencies grid: [PrcFmt]
  ) -> [PrcFmt] {
    let binHz = PrcFmt(fr.sampleRate) / PrcFmt(fr.fftSize)
    var out = [PrcFmt](repeating: 0, count: grid.count)
    for (i, f) in grid.enumerated() {
      let bin = Int((f / binHz).rounded())
      let clamped = max(0, min(fr.bins - 1, bin))
      out[i] = fr.magnitudeDB(at: clamped)
    }
    return out
  }

  // MARK: - Fit driver

  public static func fit(
    measuredMagnitudeDB: [PrcFmt],
    frequencies: [PrcFmt],
    target: TargetCurve,
    sampleRate: Int,
    options: Options = Options()
  ) -> [BiquadParameters] {
    precondition(measuredMagnitudeDB.count == frequencies.count)
    let n = frequencies.count
    guard n > 4 else { return [] }

    // Baseline residual = measured − target. Positive ⇒ measured is
    // too loud at that frequency; band should attenuate.
    let rawResidual: [PrcFmt] = (0..<n).map {
      measuredMagnitudeDB[$0] - target.evaluate(atFreqHz: frequencies[$0])
    }

    // Smooth the residual on log-frequency before fitting. AutoEQ-
    // style: 1/12 octave Gaussian midband, transitioning to 2 octaves
    // above 8 kHz so high-frequency measurement noise doesn't drive
    // the optimizer. The optimizer minimises against the smoothed
    // curve; the user can still see the raw measurement in the
    // overlay.
    let baseResidual = smoothLogOctave(
      rawResidual, frequencies: frequencies,
      midOctaves: options.smoothingOctaves,
      trebleOctaves: options.trebleSmoothingOctaves,
      transitionLowHz: options.smoothingTransitionLow,
      transitionHighHz: options.smoothingTransitionHigh)

    // Phase 1: seed.
    var bands = seedBands(
      baseResidual: baseResidual,
      frequencies: frequencies,
      sampleRate: sampleRate,
      options: options)

    // Phase 2: coordinate descent.
    for _ in 0..<options.refinementIterations {
      var maxChange: PrcFmt = 0
      for i in 0..<bands.count {
        // Residual the i-th band sees: baseline plus contributions
        // from all OTHER bands. (When this band's gain is set to its
        // ideal value, total residual reaches the local minimum.)
        var rwb = baseResidual
        for (j, b) in bands.enumerated() where j != i {
          accumulateBandResponse(
            band: b, frequencies: frequencies,
            sampleRate: sampleRate, into: &rwb)
        }

        let original = bands[i]
        let optimized = optimizeBand(
          original,
          residualWithoutBand: rwb,
          frequencies: frequencies,
          sampleRate: sampleRate,
          options: options)
        bands[i] = optimized
        maxChange = max(maxChange, parameterDelta(original, optimized))
      }
      // Settled — no band moved more than 0.1% / 0.05 dB / 0.1% Q.
      if maxChange < 0.001 { break }
    }

    // Phase 3: cleanup.
    bands.removeAll { abs($0.gain ?? 0) < options.dropGainDB }
    return bands
  }

  // MARK: - Phase 1: seed bands

  /// Greedy seed: optional shelves at the endpoints (median-residual
  /// in each edge band) plus peaking placements on the largest
  /// residual peaks. The optimizer in Phase 2 takes over from here.
  private static func seedBands(
    baseResidual: [PrcFmt],
    frequencies: [PrcFmt],
    sampleRate: Int,
    options: Options
  ) -> [BiquadParameters] {
    var bands: [BiquadParameters] = []
    var residual = baseResidual

    // Modal mode suppresses the low shelf — its corner sits inside
    // the modal region, where shelves do more harm than good
    // (they can't surgically cut a single mode without lifting / dropping
    // the whole bass shelf above it).
    let suppressLowShelf =
      options.modalMode
      && options.lowShelfFreqHz <= options.schroederHz
    if options.addEndpointShelves, !suppressLowShelf {
      if let lo = seedShelf(
        type: .lowshelf,
        edgeBand: (options.minFreqHz, options.lowShelfFreqHz),
        cornerHz: options.lowShelfFreqHz,
        residual: residual,
        frequencies: frequencies,
        options: options)
      {
        bands.append(lo)
        accumulateBandResponse(
          band: lo, frequencies: frequencies,
          sampleRate: sampleRate, into: &residual)
      }
      if let hi = seedShelf(
        type: .highshelf,
        edgeBand: (options.highShelfFreqHz, options.maxFreqHz),
        cornerHz: options.highShelfFreqHz,
        residual: residual,
        frequencies: frequencies,
        options: options)
      {
        bands.append(hi)
        accumulateBandResponse(
          band: hi, frequencies: frequencies,
          sampleRate: sampleRate, into: &residual)
      }
    }

    // Greedy peaking seed on the remaining residual.
    let peakBudget = max(0, options.bandCount - bands.count)
    for _ in 0..<peakBudget {
      guard
        let peak = seedPeak(
          residual: residual,
          frequencies: frequencies,
          options: options)
      else { break }
      bands.append(peak)
      accumulateBandResponse(
        band: peak, frequencies: frequencies,
        sampleRate: sampleRate, into: &residual)
    }
    return bands
  }

  private static func seedPeak(
    residual: [PrcFmt],
    frequencies: [PrcFmt],
    options: Options
  ) -> BiquadParameters? {
    // In modal mode below the Schroeder frequency, only POSITIVE
    // residual peaks are correctable — boosting a null doesn't fill
    // it, the mode is still there. Skip negative-residual minima in
    // that range so the optimizer doesn't waste a band trying.
    var bestIdx = -1
    var bestAbs: PrcFmt = 0
    for i in 0..<residual.count {
      let f = frequencies[i]
      if f < options.minFreqHz || f > options.maxFreqHz { continue }
      let v = residual[i]
      if options.modalMode, f <= options.schroederHz, v <= 0 { continue }
      let a = abs(v)
      if a > bestAbs {
        bestAbs = a
        bestIdx = i
      }
    }
    if bestIdx < 0 || bestAbs < options.convergenceDB { return nil }

    // Estimate Q from the −3 dB-relative bandwidth around the peak.
    let peak = residual[bestIdx]
    let halfTarget = abs(peak) * 0.5
    let sign = peak >= 0 ? 1.0 : -1.0
    var l = bestIdx
    while l > 0, residual[l - 1] * sign >= halfTarget { l -= 1 }
    var r = bestIdx
    while r < residual.count - 1, residual[r + 1] * sign >= halfTarget { r += 1 }
    let f0 = frequencies[bestIdx]
    let bw = max(frequencies[r] - frequencies[l], f0 * 0.05)
    // Modal cuts get a higher Q floor — modes are sharp and cutting
    // with low-Q would attenuate musical content around the mode
    // unnecessarily.
    let modalActive = options.modalMode && f0 <= options.schroederHz
    let qFloor = modalActive ? max(options.minQ, options.modalMinQ) : options.minQ
    let q = max(qFloor, min(options.maxQ, f0 / bw))
    let gain = max(-options.maxGainDB, min(options.maxGainDB, -peak))
    return BiquadParameters(type: .peaking, freq: f0, gain: gain, q: q)
  }

  private static func seedShelf(
    type: BiquadType,
    edgeBand: (PrcFmt, PrcFmt),
    cornerHz: PrcFmt,
    residual: [PrcFmt],
    frequencies: [PrcFmt],
    options: Options
  ) -> BiquadParameters? {
    let (lo, hi) = edgeBand
    var samples: [PrcFmt] = []
    for i in 0..<frequencies.count where frequencies[i] >= lo && frequencies[i] <= hi {
      samples.append(residual[i])
    }
    guard samples.count >= 4 else { return nil }
    samples.sort()
    let median = samples[samples.count / 2]
    if abs(median) < options.convergenceDB { return nil }
    let gain = max(-options.maxGainDB, min(options.maxGainDB, -median))
    return BiquadParameters(type: type, freq: cornerHz, gain: gain, q: 0.71)
  }

  // MARK: - Phase 2: per-band optimization

  /// One full optimization pass over a single band: golden-section
  /// search on each parameter while the others are held fixed. Two
  /// inner cycles are usually enough since the parameters interact
  /// loosely.
  private static func optimizeBand(
    _ band: BiquadParameters,
    residualWithoutBand rwb: [PrcFmt],
    frequencies: [PrcFmt],
    sampleRate: Int,
    options: Options
  ) -> BiquadParameters {
    var current = band

    for _ in 0..<2 {
      current = optimizeGain(
        current, rwb: rwb, frequencies: frequencies,
        sampleRate: sampleRate, options: options)
      if current.type != .lowshelf, current.type != .highshelf {
        // Peakings get freq + Q optimized too.
        current = optimizeQ(
          current, rwb: rwb, frequencies: frequencies,
          sampleRate: sampleRate, options: options)
        current = optimizeFreq(
          current, rwb: rwb, frequencies: frequencies,
          sampleRate: sampleRate, options: options)
      } else {
        // Shelves: corner frequency wanders but Q stays in a tight
        // band — Q on a shelf has a subtler effect and easily
        // overshoots into ringing if optimized aggressively.
        current = optimizeQ(
          current, rwb: rwb, frequencies: frequencies,
          sampleRate: sampleRate, options: options,
          minQOverride: 0.4, maxQOverride: 0.7)
        current = optimizeFreq(
          current, rwb: rwb, frequencies: frequencies,
          sampleRate: sampleRate, options: options)
      }
    }
    return current
  }

  private static func optimizeGain(
    _ band: BiquadParameters,
    rwb: [PrcFmt],
    frequencies: [PrcFmt],
    sampleRate: Int,
    options: Options
  ) -> BiquadParameters {
    let result = goldenSectionSearch(
      lo: -options.maxGainDB,
      hi: options.maxGainDB,
      tolerance: 0.02,
      logSpace: false
    ) { gain in
      var b = band
      b.gain = gain
      return cost(band: b, rwb: rwb, frequencies: frequencies, sampleRate: sampleRate)
    }
    var b = band
    b.gain = result
    return b
  }

  private static func optimizeQ(
    _ band: BiquadParameters,
    rwb: [PrcFmt],
    frequencies: [PrcFmt],
    sampleRate: Int,
    options: Options,
    minQOverride: PrcFmt? = nil,
    maxQOverride: PrcFmt? = nil
  ) -> BiquadParameters {
    let qLo = minQOverride ?? options.minQ
    let qHi = maxQOverride ?? options.maxQ
    let result = goldenSectionSearch(
      lo: qLo, hi: qHi, tolerance: 0.005, logSpace: true
    ) { q in
      var b = band
      b.q = q
      return cost(band: b, rwb: rwb, frequencies: frequencies, sampleRate: sampleRate)
    }
    var b = band
    b.q = result
    return b
  }

  private static func optimizeFreq(
    _ band: BiquadParameters,
    rwb: [PrcFmt],
    frequencies: [PrcFmt],
    sampleRate: Int,
    options: Options
  ) -> BiquadParameters {
    // Search ±1 octave around current freq (log-space).
    let f0 = band.freq ?? 1000
    let lo = max(options.minFreqHz, f0 / 2)
    let hi = min(options.maxFreqHz, f0 * 2)
    if hi <= lo { return band }
    let result = goldenSectionSearch(
      lo: lo, hi: hi, tolerance: 0.001, logSpace: true
    ) { freq in
      var b = band
      b.freq = freq
      return cost(band: b, rwb: rwb, frequencies: frequencies, sampleRate: sampleRate)
    }
    var b = band
    b.freq = result
    return b
  }

  /// Sum of squared residuals after subtracting the band's
  /// frequency response from the residual. Smaller is better.
  private static func cost(
    band: BiquadParameters,
    rwb: [PrcFmt],
    frequencies: [PrcFmt],
    sampleRate: Int
  ) -> PrcFmt {
    guard let coeffs = BiquadCoefficients.compute(parameters: band, sampleRate: sampleRate)
    else { return PrcFmt.infinity }
    var total: PrcFmt = 0
    for i in 0..<frequencies.count {
      // Post-correction residual at f, ideal value is 0:
      //   rwb[f]      = baseResidual + sum_{j != i} bandResponse_j[f]
      //   final[f]    = rwb[f] + bandResponse_i[f]
      // The cost is the squared L2 norm of `final`.
      let r = rwb[i] + coeffs.gainDB(atFreqHz: frequencies[i], sampleRate: sampleRate)
      total += r * r
    }
    return total
  }

  /// Golden-section search for a 1-D unimodal minimum. Returns the
  /// argument that minimizes `f`. `tolerance` is the relative
  /// fractional width of the bracket at termination (in linear or
  /// log space depending on `logSpace`).
  private static func goldenSectionSearch(
    lo: PrcFmt, hi: PrcFmt,
    tolerance: PrcFmt,
    logSpace: Bool,
    _ f: (PrcFmt) -> PrcFmt
  ) -> PrcFmt {
    let phi: PrcFmt = (sqrt(5.0) - 1.0) / 2.0
    var a: PrcFmt = logSpace ? log10(lo) : lo
    var b: PrcFmt = logSpace ? log10(hi) : hi
    if a >= b { return logSpace ? pow(10, a) : a }
    var x1 = b - phi * (b - a)
    var x2 = a + phi * (b - a)
    var v1 = f(logSpace ? pow(10, x1) : x1)
    var v2 = f(logSpace ? pow(10, x2) : x2)

    var iterations = 0
    while abs(b - a) > tolerance, iterations < 40 {
      iterations += 1
      if v1 < v2 {
        b = x2
        x2 = x1
        v2 = v1
        x1 = b - phi * (b - a)
        v1 = f(logSpace ? pow(10, x1) : x1)
      } else {
        a = x1
        x1 = x2
        v1 = v2
        x2 = a + phi * (b - a)
        v2 = f(logSpace ? pow(10, x2) : x2)
      }
    }
    let mid = (a + b) / 2
    return logSpace ? pow(10, mid) : mid
  }

  // MARK: - Helpers

  /// Accumulate the band's frequency response into the running
  /// residual. The band's contribution moves the residual toward
  /// zero (when its gain is set correctly): with a positive raw
  /// residual at f, the band has a negative gain and its response is
  /// negative there, so adding it brings the residual down to zero.
  ///
  /// (Earlier this function subtracted instead of adding, which was
  /// a sign bug — every iteration drove the residual *away* from
  /// zero, so the optimizer kept stacking bands at the same peak.)
  private static func accumulateBandResponse(
    band: BiquadParameters,
    frequencies: [PrcFmt],
    sampleRate: Int,
    into residual: inout [PrcFmt]
  ) {
    guard let coeffs = BiquadCoefficients.compute(parameters: band, sampleRate: sampleRate)
    else { return }
    for i in 0..<frequencies.count {
      residual[i] += coeffs.gainDB(atFreqHz: frequencies[i], sampleRate: sampleRate)
    }
  }

  // MARK: - Smoothing
  //
  // Gaussian-weighted moving average on the log-frequency grid.
  // Width is specified in octaves and varies smoothly with frequency
  // via a sigmoid blend between the midband and treble values, so
  // narrow modal features stay sharp while high-frequency
  // measurement noise gets averaged out.

  /// Uniform-width fractional-octave smoothing — simpler signature
  /// for callers (display layer) that don't need the
  /// midband/treble blend.
  public static func smoothLogOctave(
    _ values: [PrcFmt],
    frequencies: [PrcFmt],
    octaves: PrcFmt
  ) -> [PrcFmt] {
    smoothLogOctave(
      values,
      frequencies: frequencies,
      midOctaves: octaves,
      trebleOctaves: octaves,
      transitionLowHz: 1,
      transitionHighHz: 2)
  }

  public static func smoothLogOctave(
    _ values: [PrcFmt],
    frequencies: [PrcFmt],
    midOctaves: PrcFmt,
    trebleOctaves: PrcFmt,
    transitionLowHz: PrcFmt,
    transitionHighHz: PrcFmt
  ) -> [PrcFmt] {
    precondition(values.count == frequencies.count)
    let n = values.count
    if n == 0 { return values }
    let logF = frequencies.map { log10(max($0, 1)) }
    let log10_2 = log10(2.0)
    let lowLog = log10(transitionLowHz)
    let highLog = log10(transitionHighHz)

    var out = [PrcFmt](repeating: 0, count: n)
    for i in 0..<n {
      // Sigmoid blend t ∈ [0, 1] across the transition band.
      let t: PrcFmt
      if logF[i] <= lowLog {
        t = 0
      } else if logF[i] >= highLog {
        t = 1
      } else {
        let u = (logF[i] - lowLog) / (highLog - lowLog)
        t = u * u * (3.0 - 2.0 * u)  // smoothstep
      }
      let octWidth = midOctaves + t * (trebleOctaves - midOctaves)
      let sigma = octWidth * log10_2 / 2.0  // half-width at 1σ
      let radius = 3.0 * sigma  // truncate kernel at 3σ
      var sum: PrcFmt = 0
      var wsum: PrcFmt = 0
      for j in 0..<n {
        let d = logF[j] - logF[i]
        if abs(d) > radius { continue }
        let w = exp(-0.5 * d * d / (sigma * sigma))
        sum += w * values[j]
        wsum += w
      }
      out[i] = wsum > 0 ? sum / wsum : values[i]
    }
    return out
  }

  /// Convergence metric: max relative change in any parameter.
  /// Frequency / Q changes are scaled by their nominal magnitudes;
  /// gain change is treated as absolute (dB).
  private static func parameterDelta(
    _ a: BiquadParameters, _ b: BiquadParameters
  ) -> PrcFmt {
    let af = a.freq ?? 1
    let bf = b.freq ?? 1
    let df = abs(af - bf) / max(1.0, af)
    let dg = abs((a.gain ?? 0) - (b.gain ?? 0)) / 10.0  // dB / 10 dB
    let aq = a.q ?? 1
    let bq = b.q ?? 1
    let dq = abs(aq - bq) / max(0.1, aq)
    return max(df, max(dg, dq))
  }
}
