// Room-correction measurement state.
//
// Holds whatever the user has measured (or synthesised in mock mode):
// the impulse response, the derived frequency response, the target
// curve, and the parametric-EQ chain produced by `PEQAutoFit`. Plot
// views read these as the source of truth; controls mutate them.
//
// Mock mode runs the full sweep → deconvolve → FR → fit pipeline on
// a synthetic "system" so the UI can be developed and exercised
// before the real microphone-input path lands.

import DSPAudio
import DSPBackend
import DSPConfig
import DSPFilters
import DSPMeasurement
import Foundation
import Observation

/// What the user told us this position represents. Drives the
/// subwoofer-crossover assistant: when the session has at least one
/// `.mains` and one `.subwoofer` position, the assistant can compute
/// the time-of-flight delay between them and suggest crossover
/// settings. Default is `.full` (whole speaker chain together) which
/// is what a single-source measurement captures.
enum MeasurementChannelKind: String, Codable, Sendable, CaseIterable, Identifiable {
  case full = "Full Range"
  case mains = "Mains Only"
  case subwoofer = "Subwoofer Only"
  var id: String { rawValue }
}

/// One captured / imported measurement at a single mic position.
struct MeasurementPosition: Identifiable, Sendable {
  let id: UUID
  var name: String
  var fr: FrequencyResponse
  /// Optional time-domain IR. Only populated when the position came
  /// from a sweep capture (mock or real). FRD-imported positions
  /// have no IR, so the IR/Group-Delay panes fall back to the most
  /// recent capture instead.
  var ir: ImpulseResponse?
  /// When `false`, the position is excluded from the average but
  /// remains in the list (lets the user A/B compare).
  var isEnabled: Bool
  /// What part of the system this position represents. The
  /// subwoofer-crossover assistant compares mains-only and
  /// subwoofer-only IRs to suggest a crossover.
  var kind: MeasurementChannelKind

  init(
    name: String, fr: FrequencyResponse, ir: ImpulseResponse? = nil,
    isEnabled: Bool = true, kind: MeasurementChannelKind = .full
  ) {
    self.id = UUID()
    self.name = name
    self.fr = fr
    self.ir = ir
    self.isEnabled = isEnabled
    self.kind = kind
  }
}

enum FIRKind: String, CaseIterable, Identifiable, Sendable {
  /// IIR-equivalent FIR derived from the editable EQ chain. Same
  /// magnitude as the EQ; min-phase paired with that magnitude.
  case minimumPhase = "Min-phase"
  /// Same magnitude as the EQ chain, with constant group delay
  /// (`taps / 2 / sampleRate` of latency). Pre-rings on transients.
  case linearPhase = "Linear-phase"
  /// Bypasses the EQ chain. Designs the FIR straight from the
  /// complex measured FR — corrects both magnitude AND phase
  /// (excess phase included). Constant group delay = taps / 2.
  case measurementDriven = "From measurement"
  var id: String { rawValue }

  /// True iff the design path consumes the editable EQ chain.
  /// Measurement-driven design is independent of the EQ.
  var derivedFromEQ: Bool {
    self != .measurementDriven
  }
}

@MainActor
@Observable
final class MeasurementSession {
  // MARK: - Inputs / sweep parameters

  var sweepF1: Double = 20.0
  var sweepF2: Double = 20_000.0
  var sweepDurationSeconds: Double = 1.0
  var sampleRate: Int = 48_000

  // MARK: - Target curve and fit settings

  var targetPreset: TargetCurve.Preset = .flat
  /// Custom curve breakpoints — used when the user drags handles in
  /// the editor. Falls back to `targetPreset.curve` when empty.
  var customTarget: TargetCurve? = nil
  /// Effective target curve — custom overrides preset.
  var targetCurve: TargetCurve {
    if let custom = customTarget, !custom.breakpoints.isEmpty {
      return custom
    }
    return targetPreset.curve
  }

  var bandCount: Int = 8
  var maxGainDB: Double = 12.0
  /// When true, restrict bands below `schroederHz` to negative
  /// gain (cuts only) and high Q. Standard practice for modal
  /// (low-frequency) room correction.
  var modalMode: Bool = false
  /// Schroeder frequency — boundary between the modal region and
  /// the diffuse field. Default 200 Hz suits typical living rooms;
  /// larger rooms or treated spaces transition lower.
  var schroederHz: Double = 200
  /// Minimum Q for modal-region bands. Higher Q means narrower,
  /// more surgical cuts — robust against placement variation but
  /// less effective on broad modal humps. 2.0 is a balanced default.
  var modalMinQ: Double = 2.0

  // MARK: - Frequency-dependent windowing (FDW)

  enum FDWCycles: String, CaseIterable, Identifiable, Sendable {
    case off = "Off"
    case cycles1 = "1 cycle"
    case cycles5 = "5 cycles"
    case cycles10 = "10 cycles"
    case cycles15 = "15 cycles"
    var id: String { rawValue }
    var cycles: Double? {
      switch self {
      case .off: return nil
      case .cycles1: return 1.0
      case .cycles5: return 5.0
      case .cycles10: return 10.0
      case .cycles15: return 15.0
      }
    }
  }

  /// Frequency-dependent windowing width. When set, replaces the fixed
  /// Tukey window with a variable-width Hann window during averaging.
  var fdwCycles: FDWCycles = .off {
    didSet {
      if oldValue != fdwCycles {
        recomputeAverage()
      }
    }
  }

  // MARK: - Display smoothing

  /// Fractional-octave widths for the magnitude display. Picked to
  /// match REW's standard set. `nil` (off) shows the raw FR.
  enum DisplaySmoothing: String, CaseIterable, Identifiable {
    case off = "Off"
    case oct1over3 = "1/3 oct"
    case oct1over6 = "1/6 oct"
    case oct1over12 = "1/12 oct"
    case oct1over24 = "1/24 oct"
    var id: String { rawValue }
    var width: Double? {
      switch self {
      case .off: return nil
      case .oct1over3: return 1.0 / 3.0
      case .oct1over6: return 1.0 / 6.0
      case .oct1over12: return 1.0 / 12.0
      case .oct1over24: return 1.0 / 24.0
      }
    }
  }

  /// Smoothing applied to the **displayed** measured curve only.
  /// Doesn't affect the auto-fitter's input (the fitter does its
  /// own AutoEQ-style smoothing internally).
  var displaySmoothing: DisplaySmoothing = .oct1over6

  /// `measuredMagDB` smoothed at the user's chosen octave width, for
  /// the magnitude overlay. Computed lazily on access — cheap enough
  /// (256 grid points) that we don't need to cache the result.
  var displayedMagDB: [Double] {
    guard let width = displaySmoothing.width else { return measuredMagDB }
    return PEQAutoFit.smoothLogOctave(
      measuredMagDB, frequencies: grid, octaves: width)
  }

  // MARK: - Microphone capture

  /// Name of the input device to use for sweep capture (`nil` =
  /// system default). Persisted across launches so the user doesn't
  /// re-pick every session.
  var selectedMicName: String?
  /// Name of the output device the sweep plays through (`nil` =
  /// system default). Set when the listening setup's speakers are
  /// on a different DAC than the system default.
  var selectedOutputName: String?
  /// Output channel index (0-based) the sweep is routed to. `-1`
  /// means "all channels" (mono fan-out across the whole bus). Used
  /// to test one speaker at a time (e.g. left-only, right-only,
  /// sub-LFE-only) so the captured response reflects that speaker
  /// alone.
  var selectedOutputChannel: Int = -1
  /// Input channel index (0-based) recorded from. Defaults to
  /// channel 0; surfaced so users with stereo / multi-mic interfaces
  /// can pick the calibrated capsule.
  var selectedInputChannel: Int = 0
  /// Set during a capture so the UI can disable the button +
  /// surface progress.
  var isCapturing: Bool = false

  /// Run a real measurement: play the sweep through the system
  /// output, record from the selected mic, deconvolve, and append a
  /// `MeasurementPosition`. Mirrors the API of
  /// `generateMockMeasurement(append:)` so the UI can call either
  /// without branching.
  ///
  /// Async because the AVAudioEngine play+record blocks for sweep
  /// duration + a small grace window. Errors flow into
  /// `session.status` so the UI shows them inline.
  func captureMeasurement(append: Bool = false) async {
    guard !isCapturing else { return }
    isCapturing = true
    defer { isCapturing = false }
    if !append {
      positions.removeAll()
    }
    status = "Capturing — playing sweep…"

    let (sweep, inverse) = SweepGenerator.sweepAndInverse(
      f1: sweepF1, f2: sweepF2,
      durationSeconds: sweepDurationSeconds,
      sampleRate: sampleRate,
      fadeInSeconds: 0.02, fadeOutSeconds: 0.02)

    do {
      let result = try await SweepRecorder.capture(
        sweep: sweep,
        inverse: inverse,
        sampleRate: sampleRate,
        inputDeviceName: selectedMicName,
        outputDeviceName: selectedOutputName,
        inputChannel: selectedInputChannel,
        outputChannel: selectedOutputChannel)
      let ir = SweepDeconvolver.deconvolve(
        captured: result.captured,
        f1: sweepF1, f2: sweepF2,
        durationSeconds: sweepDurationSeconds,
        sampleRate: sampleRate)
      let windowed = ir.windowed(
        leftSamples: sampleRate / 200,
        rightSamples: sampleRate / 5,
        taperFraction: 0.1)
      let fr = FrequencyResponse.from(impulseResponse: windowed)

      let positionName = "Position \(positions.count + 1)"
      let position = MeasurementPosition(name: positionName, fr: fr, ir: windowed)
      positions.append(position)
      recomputeAverage()

      if !append {
        if let existing = correctionPreset {
          existing.bands = []
          existing.preampGain = 0
        } else {
          self.correctionPreset = EQPreset(
            name: "Room Correction", preampGain: 0, bands: [])
        }
      }

      let latencyMs = Double(result.roundTripSamples) / Double(sampleRate) * 1000.0
      let peakDB =
        result.peakAbsolute > 0
        ? String(format: "%.1f dBFS", 20.0 * log10(result.peakAbsolute))
        : "—"
      let warning =
        result.peakAbsolute > 0.95
        ? " · clipping risk!"
        : (result.peakAbsolute < 0.05 ? " · low signal" : "")
      status =
        "Captured \(positionName) — peak \(peakDB)"
        + ", round-trip \(String(format: "%.0f", latencyMs)) ms\(warning)."
    } catch let err as SweepRecorder.CaptureError {
      status = "Capture failed: \(err)"
    } catch {
      status = "Capture failed: \(error.localizedDescription)"
    }
  }

  // MARK: - Microphone calibration

  /// Loaded calibration curve (mic FR, in dB vs Hz). Subtracted from
  /// `measuredMagDB` before the auto-fitter sees it, so the fit
  /// targets the system's actual response and not the mic's own
  /// colouration.
  var calibration: CalibrationCurve?
  /// Display-only path of the loaded calibration file.
  var calibrationPath: String?

  // MARK: - FIR design settings

  var firKind: FIRKind = .minimumPhase
  /// FFT size used for the FIR design. Controls frequency resolution
  /// (`sampleRate / fftSize` Hz at DC) and IR length / latency.
  var firTapCount: Int = 8192
  /// Phase blend for the measurement-driven FIR mode (0 = min-phase
  /// / ~0 latency / no pre-ring; 1 = linear-phase / full latency /
  /// full pre-ring). Ignored for the EQ-derived modes.
  var firPhaseBlend: Double = 1.0
  /// Path of the most recently generated IR file, if any.
  var generatedFIRPath: String?

  // MARK: - Multi-position state

  /// Captured / imported positions. Adding a position recomputes the
  /// averaged FR that downstream code reads via `measuredFR`.
  var positions: [MeasurementPosition] = []

  // MARK: - Outputs (populated after a measurement / fit run)

  var measuredIR: ImpulseResponse?
  var measuredFR: FrequencyResponse?
  /// Magnitude of `measuredFR` resampled onto the analysis grid; cached
  /// so the plot view doesn't re-walk the FFT bins on every redraw.
  var measuredMagDB: [Double] = []
  /// Log-spaced frequency grid the analysis runs on — shared by the
  /// plot views so measured / target / corrected overlays are
  /// strictly comparable.
  var grid: [Double] = []

  /// Editable EQ produced by `runFit()`. Source of truth for both the
  /// Magnitude pane (rendered through `EQFrequencyResponseView` so
  /// the user can drag bands directly), the FIR designer, and the
  /// "Apply to EQ Preset" handoff. Lives outside `pipeline.eqPresets`
  /// — it's a working copy until the user chooses to apply it.
  var correctionPreset: EQPreset?

  /// User-facing status string. Cheap progress channel; the heavy
  /// work is done in synchronous calls so we don't need a richer
  /// async state machine here.
  var status: String = "No measurement loaded."

  // MARK: - Operations

  /// Generate a synthetic "measured" response by playing a sweep
  /// through a hand-crafted biquad chain that approximates a typical
  /// untreated listening room (low-frequency room modes + speaker
  /// roll-off). The result is round-tripped through the actual
  /// `SweepDeconvolver` and `FrequencyResponse` plumbing so the UI
  /// exercises the same code path it will use with a real
  /// microphone.
  func generateMockMeasurement(append: Bool = false) {
    status = "Generating mock measurement…"
    if !append {
      positions.removeAll()
    }

    let mockChain = randomMockSystem()
    let (sweep, _) = SweepGenerator.sweepAndInverse(
      f1: sweepF1, f2: sweepF2,
      durationSeconds: sweepDurationSeconds,
      sampleRate: sampleRate,
      fadeInSeconds: 0.02, fadeOutSeconds: 0.02)

    var captured = sweep
    for params in mockChain {
      guard let coeffs = BiquadCoefficients.compute(parameters: params, sampleRate: sampleRate)
      else { continue }
      let filter = BiquadFilter(coefficients: coeffs)
      captured.withUnsafeMutableBufferPointer { buf in
        filter.process(waveform: buf)
      }
    }

    let ir = SweepDeconvolver.deconvolve(
      captured: captured,
      f1: sweepF1, f2: sweepF2,
      durationSeconds: sweepDurationSeconds,
      sampleRate: sampleRate)
    let windowed = ir.windowed(
      leftSamples: sampleRate / 200,
      rightSamples: sampleRate / 5,
      taperFraction: 0.1)
    let fr = FrequencyResponse.from(impulseResponse: windowed)

    let positionName = "Position \(positions.count + 1)"
    let position = MeasurementPosition(name: positionName, fr: fr, ir: windowed)
    positions.append(position)
    recomputeAverage()

    if !append {
      // Reset the editable EQ on a fresh measurement set; appending
      // keeps the user's in-progress correction.
      if let existing = correctionPreset {
        existing.bands = []
        existing.preampGain = 0
      } else {
        self.correctionPreset = EQPreset(
          name: "Room Correction", preampGain: 0, bands: [])
      }
    }
    self.status =
      positions.count == 1
      ? "Mock measurement ready."
      : "Added \(positionName) — averaging \(enabledPositionCount) of \(positions.count) positions."
  }

  // MARK: - Position management

  private var enabledPositionCount: Int {
    positions.filter(\.isEnabled).count
  }

  func togglePosition(id: UUID) {
    guard let idx = positions.firstIndex(where: { $0.id == id }) else { return }
    positions[idx].isEnabled.toggle()
    recomputeAverage()
  }

  func removePosition(id: UUID) {
    positions.removeAll { $0.id == id }
    if positions.isEmpty {
      reset()
    } else {
      recomputeAverage()
    }
  }

  func setPositionKind(id: UUID, kind: MeasurementChannelKind) {
    guard let idx = positions.firstIndex(where: { $0.id == id }) else { return }
    positions[idx].kind = kind
  }

  // MARK: - Subwoofer crossover assist

  /// Available iff at least one `.mains` and one `.subwoofer`
  /// position are loaded. Picks the most recent pair.
  var subwooferAssistAvailable: Bool {
    positions.contains(where: { $0.kind == .mains && $0.ir != nil })
      && positions.contains(where: { $0.kind == .subwoofer && $0.ir != nil })
  }

  /// Run the assistant. Returns `nil` when the necessary
  /// measurements aren't loaded.
  func computeSubwooferRecommendation() -> SubwooferRecommendation? {
    guard let mainsIR = positions.last(where: { $0.kind == .mains })?.ir,
      let subIR = positions.last(where: { $0.kind == .subwoofer })?.ir
    else {
      status = "Need one mains-only and one subwoofer-only position to compute crossover."
      return nil
    }
    return SubwooferAssist.recommend(
      mainsIR: mainsIR, subIR: subIR)
  }

  /// RMS-average the magnitudes of all enabled positions and update
  /// `measuredFR` / `measuredMagDB`. Phase is taken from the most
  /// recent position; cross-position phase averaging is meaningless
  /// for spatial sampling so we don't try.
  ///
  /// Falls back to the most recent single position when only one is
  /// enabled, which matches the single-measurement workflow.
  func recomputeAverage() {
    let enabled = positions.filter(\.isEnabled)
    guard let first = enabled.first else {
      // Nothing to average — clear the derived state.
      measuredFR = nil
      measuredIR = nil
      measuredMagDB = []
      grid = []
      return
    }
    let g = PEQAutoFit.logFrequencyGrid(fMin: 20, fMax: 20_000, count: 256)

    // Helper to extract the effective FrequencyResponse for a position,
    // applying FDW if requested and available.
    let effectiveFR: (MeasurementPosition) -> FrequencyResponse = { p in
      if let cycles = self.fdwCycles.cycles, let ir = p.ir {
        return FrequencyResponse.fdw(impulseResponse: ir, cycles: cycles)
      }
      return p.fr
    }

    let combined: [Double]
    if enabled.count == 1 {
      combined = PEQAutoFit.sampleMagnitudeDB(of: effectiveFR(first), atFrequencies: g)
    } else {
      // RMS magnitude average on the analysis grid (linear-magnitude
      // squared mean — the correct way to combine spatial samples).
      var sumPow = [Double](repeating: 0, count: g.count)
      for p in enabled {
        let dB = PEQAutoFit.sampleMagnitudeDB(of: effectiveFR(p), atFrequencies: g)
        for i in 0..<g.count {
          let lin = pow(10.0, dB[i] / 20.0)
          sumPow[i] += lin * lin
        }
      }
      var avgDB = [Double](repeating: 0, count: g.count)
      let n = Double(enabled.count)
      for i in 0..<g.count {
        let mean = (sumPow[i] / n).squareRoot()
        avgDB[i] = 20.0 * log10(max(mean, 1e-12))
      }
      combined = avgDB
    }

    // Apply mic calibration, then centre the curve on its in-band
    // median so the auto-fitter sees response *shape*, not absolute
    // level. Without this, deconvolved sweeps (60–90 dB absolute
    // range) saturate every fitted band at the gain cap.
    let calibrated = MeasurementSession.applyCalibration(
      combined, grid: g, calibration: calibration)
    let centred = MeasurementSession.levelNormalize(calibrated, grid: g)

    self.measuredFR = effectiveFR(first)  // phase pane uses the effective response
    self.measuredIR = enabled.last?.ir
    self.grid = g
    self.measuredMagDB = centred
  }

  /// Import an FRD file as a new position. Synthesizes a
  /// `FrequencyResponse` directly from the file's frequency points
  /// (re-binned onto a uniform FFT grid via interpolation) so the
  /// rest of the pipeline doesn't need a special-case path.
  func importPositionFRD(from path: String) {
    do {
      let curve = try CalibrationCurve.load(at: path)
      let fftSize = 4096
      let bins = fftSize / 2 + 1
      var re = [Double](repeating: 0, count: bins)
      var im = [Double](repeating: 0, count: bins)
      let binHz = Double(sampleRate) / Double(fftSize)
      for k in 0..<bins {
        let f = Double(k) * binHz
        let dB = curve.magnitude(at: max(f, 1))
        let mag = pow(10.0, dB / 20.0)
        let phaseDeg = curve.phase(at: max(f, 1))
        let phaseRad = phaseDeg * .pi / 180.0
        re[k] = mag * cos(phaseRad)
        im[k] = mag * sin(phaseRad)
      }
      let fr = FrequencyResponse(real: re, imag: im, sampleRate: sampleRate, fftSize: fftSize)
      let name = (path as NSString).lastPathComponent
      let position = MeasurementPosition(name: name, fr: fr, ir: nil)
      positions.append(position)
      recomputeAverage()
      // Auto-create empty correction so the user lands in the EQ
      // editor view immediately.
      if correctionPreset == nil {
        correctionPreset = EQPreset(
          name: "Room Correction", preampGain: 0, bands: [])
      }
      status = "Imported \(name) (\(positions.count) total positions)."
    } catch {
      status = "FRD import failed: \(error)"
    }
  }

  /// Run the PEQ auto-fit against the current measurement + target.
  /// Idempotent; safe to call repeatedly with different `bandCount` /
  /// `maxGainDB`.
  func runFit() {
    guard !measuredMagDB.isEmpty else {
      status = "Run a measurement before fitting."
      return
    }
    let opts = PEQAutoFit.Options(
      bandCount: bandCount,
      maxGainDB: maxGainDB,
      modalMode: modalMode,
      schroederHz: schroederHz,
      modalMinQ: modalMinQ)
    let bands = PEQAutoFit.fit(
      measuredMagnitudeDB: measuredMagDB,
      frequencies: grid,
      target: targetCurve,
      sampleRate: sampleRate,
      options: opts)

    let eqBands = bands.compactMap(MeasurementSession.eqBand(from:))
    self.correctionPreset = EQPreset(
      name: "Room Correction",
      preampGain: -6.0,
      bands: eqBands)
    self.status = "Fit produced \(eqBands.count) band\(eqBands.count == 1 ? "" : "s")."
  }

  /// `BiquadParameters` → `EQBand` conversion. Uses the
  /// matching-rawValue convention between the two enums (`Peaking`,
  /// `Lowshelf`, …). Returns `nil` for biquad types `EQBand` doesn't
  /// support; the auto-fitter only emits peaking, so this is a thin
  /// safety net rather than a hot path.
  static func eqBand(from p: BiquadParameters) -> EQBand? {
    guard let kind = p.type else { return nil }
    guard let mapped = EQBandType(rawValue: kind.rawValue) else { return nil }

    let band = EQBand(type: mapped)
    switch mapped {
    case .free:
      band.b0 = p.b0 ?? 1.0
      band.b1 = p.b1 ?? 0.0
      band.b2 = p.b2 ?? 0.0
      band.a1 = p.a1 ?? 0.0
      band.a2 = p.a2 ?? 0.0
    case .generalNotch:
      band.freqNotch = p.freqNotch ?? 1000.0
      band.freqPole = p.freqPole ?? 1000.0
      band.normalizeAtDc = p.normalizeAtDc ?? true
    case .linkwitzTransform:
      band.freqAct = p.freqAct ?? 50.0
      band.qAct = p.qAct ?? 0.707
      band.freqTarget = p.freqTarget ?? 20.0
      band.qTarget = p.qTarget ?? 0.707
    default:
      guard let f = p.freq else { return nil }
      band.freq = f
      band.gain = p.gain ?? 0.0
      band.q = p.q ?? 0.707
    }
    return band
  }

  /// Inverse of `eqBand(from:)`: drop a band into the FIR designer's
  /// `BiquadParameters` shape.
  static func biquadParameters(from band: EQBand) -> BiquadParameters {
    guard let mapped = BiquadType(rawValue: band.type.rawValue) else {
      return BiquadParameters(type: .peaking)
    }

    var params = BiquadParameters(type: mapped)
    switch band.type {
    case .free:
      params.b0 = band.b0
      params.b1 = band.b1
      params.b2 = band.b2
      params.a1 = band.a1
      params.a2 = band.a2
    case .generalNotch:
      params.freqNotch = band.freqNotch
      params.freqPole = band.freqPole
      params.normalizeAtDc = band.normalizeAtDc
    case .linkwitzTransform:
      params.freqAct = band.freqAct
      params.qAct = band.qAct
      params.freqTarget = band.freqTarget
      params.qTarget = band.qTarget
    default:
      params.freq = band.freq
      params.gain = band.type.hasGain ? band.gain : nil
      params.q = band.type.hasQ ? band.q : nil
    }
    return params
  }

  /// Build a randomised "untreated room" biquad chain so each Mock
  /// Measurement click produces visibly different data. Structure
  /// stays plausible: speaker high-/low-pass roll-offs at the band
  /// edges, 2–4 room modes in the bass region, and 0–2 broadband
  /// dips/peaks across the mids and treble.
  private func randomMockSystem() -> [BiquadParameters] {
    var chain: [BiquadParameters] = []

    // Speaker low-end roll-off — vary cutoff 25–60 Hz.
    let hpFreq = Double.random(in: 25...60)
    chain.append(.init(type: .highpass, freq: hpFreq, q: 0.707))

    // 2–4 room modes in the bass / lower midrange.
    let modeCount = Int.random(in: 2...4)
    for _ in 0..<modeCount {
      let f = Double.random(in: 40...300)
      let gain = Double.random(in: 4...10) * (Bool.random() ? 1 : -1)
      let q = Double.random(in: 3...8)
      chain.append(.init(type: .peaking, freq: f, gain: gain, q: q))
    }

    // 1–2 broadband mid/treble shifts.
    let broadbandCount = Int.random(in: 1...2)
    for _ in 0..<broadbandCount {
      let f = pow(10, Double.random(in: log10(400)...log10(8_000)))
      let gain = Double.random(in: 2...5) * (Bool.random() ? 1 : -1)
      let q = Double.random(in: 0.8...2.0)
      chain.append(.init(type: .peaking, freq: f, gain: gain, q: q))
    }

    // High-frequency roll-off — vary cutoff 11–17 kHz.
    let lpFreq = Double.random(in: 11_000...17_000)
    chain.append(.init(type: .lowpass, freq: lpFreq, q: 0.707))
    return chain
  }

  /// Generate a FIR impulse response from the current `fittedBands`
  /// using the chosen design (`firKind`), persist it as a raw 64-bit
  /// little-endian float file, and append a `ConvolutionPreset` to
  /// `pipeline.convPresets` so it appears in the sidebar and can be
  /// wired into a Convolution stage.
  ///
  /// Files land in `~/Library/Application Support/DSPMonitor/IRs/`
  /// with a per-preset UUID so multiple generations from the same
  /// session all coexist (the user can rename the preset; the file
  /// stays put).
  ///
  /// - Returns: the new preset's `id` if the operation succeeded —
  ///   the view layer hands it to `pipeline.addConvolutionPreset`,
  ///   keeping this method free of any direct `PipelineStore`
  ///   dependency.
  @discardableResult
  func generateFIR(into pipeline: PipelineStore) -> ConvolutionPreset? {
    // Validate inputs based on which design path we'll use.
    if firKind.derivedFromEQ {
      guard let source = correctionPreset, !source.bands.isEmpty else {
        status = "Run Generate PEQ before exporting a \(firKind.rawValue) FIR."
        return nil
      }
      _ = source  // explicit use to silence unused warnings
    } else {
      guard measuredFR != nil else {
        status = "Run a measurement before exporting a measurement-driven FIR."
        return nil
      }
    }
    let fittedBands: [BiquadParameters] = (correctionPreset?.bands ?? [])
      .filter { $0.isEnabled }
      .map(MeasurementSession.biquadParameters(from:))

    let kindLabel: String
    let fileLabel: String
    switch firKind {
    case .minimumPhase:
      kindLabel = "Min-phase"
      fileLabel = "minphase"
    case .linearPhase:
      kindLabel = "Linear-phase"
      fileLabel = "linphase"
    case .measurementDriven:
      kindLabel = "Measurement-driven"
      fileLabel = "measdriven"
    }

    // Design the FIR at every standard sample rate so the engine has
    // a matching IR ready whenever the device rate switches —
    // DSP's `Conv` filter doesn't resample IRs at runtime.
    // Limit to rates ≥ 32 kHz: voice rates (8k/11k/16k/22k) aren't
    // useful for music room correction and just bloat the on-disk
    // footprint.
    let rates = CoreAudioCapabilities.standardRates.filter { $0 >= 32_000 }
    let presetID = UUID()
    var irPaths: [Int: String] = [:]
    var firstTapCount = 0

    do {
      for rate in rates {
        let ir: [PrcFmt]
        switch firKind {
        case .minimumPhase:
          let opts = FIRDesign.Options(
            fftSize: firTapCount, outputLength: firTapCount, preampDB: 0)
          ir = FIRDesign.minimumPhase(
            from: fittedBands, sampleRate: rate, options: opts)
        case .linearPhase:
          let opts = FIRDesign.Options(
            fftSize: firTapCount, outputLength: firTapCount, preampDB: 0)
          ir = FIRDesign.linearPhase(
            from: fittedBands, sampleRate: rate, options: opts)
        case .measurementDriven:
          guard let measured = measuredFR else { continue }
          let opts = FIRDesign.MeasurementDesignOptions(
            fftSize: firTapCount,
            preampDB: -6,
            maxBoostDB: maxGainDB,
            minFreqHz: 30,
            maxFreqHz: 18_000,
            phaseBlend: firPhaseBlend)
          ir = FIRDesign.fromMeasurement(
            measured: measured,
            target: targetCurve,
            designSampleRate: rate,
            options: opts)
        }
        let url = try persistIR(
          ir, label: fileLabel, presetID: presetID, sampleRate: rate)
        irPaths[rate] = url.path
        firstTapCount = ir.count
      }
    } catch {
      status = "FIR export failed: \(error)"
      return nil
    }

    let presetName = nextPresetName(in: pipeline, kind: kindLabel)
    let preset = ConvolutionPreset(
      name: presetName,
      irPaths: irPaths,
      taps: firstTapCount,
      kindLabel: kindLabel)
    pipeline.addConvolutionPreset(preset)
    generatedFIRPath = preset.irPath(forSampleRate: sampleRate)

    let rateList = rates.map { String($0 / 1000) + "k" }.joined(separator: " / ")
    status = "Saved “\(presetName)” (\(firstTapCount) taps × \(rates.count) rates: \(rateList))."
    return preset
  }

  /// Pick a sensible default name for a freshly generated preset.
  /// Walks the existing list and appends a numeric suffix if the
  /// base name is taken.
  private func nextPresetName(in pipeline: PipelineStore, kind: String) -> String {
    let base = "Room Correction (\(kind))"
    let names = Set(pipeline.convPresets.map(\.name))
    if !names.contains(base) { return base }
    var i = 2
    while names.contains("\(base) \(i)") { i += 1 }
    return "\(base) \(i)"
  }

  private func persistIR(
    _ ir: [PrcFmt], label: String, presetID: UUID, sampleRate: Int
  ) throws -> URL {
    let fm = FileManager.default
    let appSupport = try fm.url(
      for: .applicationSupportDirectory, in: .userDomainMask,
      appropriateFor: nil, create: true)
    let dir =
      appSupport
      .appendingPathComponent("DSPMonitor", isDirectory: true)
      .appendingPathComponent("IRs", isDirectory: true)
    try fm.createDirectory(at: dir, withIntermediateDirectories: true)
    let url = dir.appendingPathComponent(
      "RoomCorrection-\(label)-\(sampleRate)-\(presetID.uuidString.prefix(8)).f64")
    let data = ir.withUnsafeBufferPointer { buf -> Data in
      Data(buffer: buf)
    }
    try data.write(to: url, options: .atomic)
    return url
  }

  // MARK: - Calibration loading

  /// Load a REW-style `.frd` / `.txt` mic calibration. Re-runs the
  /// last measurement's calibration step if a measurement is
  /// already loaded, so the user can swap calibration files and
  /// see the effect immediately.
  func loadCalibration(from path: String) {
    do {
      let curve = try CalibrationCurve.load(at: path)
      self.calibration = curve
      self.calibrationPath = path
      // Re-apply to the existing measurement, if any.
      if let fr = measuredFR {
        let raw = PEQAutoFit.sampleMagnitudeDB(of: fr, atFrequencies: grid)
        self.measuredMagDB = MeasurementSession.applyCalibration(
          raw, grid: grid, calibration: curve)
      }
      let name = (path as NSString).lastPathComponent
      status = "Loaded calibration “\(name).”"
    } catch {
      status = "Calibration load failed: \(error)"
    }
  }

  /// Export the current measurement as an REW-compatible `.frd`
  /// file. Writes the unsmoothed magnitude (in dB) and wrapped phase
  /// (in degrees) at the FFT bins inside `[20 Hz, 20 kHz]`.
  /// Calibration is *not* applied to the export so the user always
  /// has the raw measurement; if they want a calibrated copy they
  /// can subtract the calibration externally or load it back in.
  func exportFRD(to path: String, includeCalibration: Bool = false) -> Bool {
    guard let fr = measuredFR else {
      status = "Run a measurement before exporting."
      return false
    }
    var freqs: [Double] = []
    var mags: [Double] = []
    var phases: [Double] = []
    let binHz = Double(fr.sampleRate) / Double(fr.fftSize)
    for k in 1..<fr.bins {
      let f = Double(k) * binHz
      if f < 20 || f > 20_000 { continue }
      freqs.append(f)
      let calOffset = (includeCalibration ? (calibration?.magnitude(at: f) ?? 0) : 0)
      mags.append(fr.magnitudeDB(at: k) - calOffset)
      phases.append(fr.phase(at: k) * 180.0 / .pi)
    }
    let curve = CalibrationCurve(
      frequencies: freqs, magnitudesDB: mags, phasesDeg: phases)
    do {
      try curve.writeFRD(
        to: path,
        comment: "Sample rate: \(sampleRate) Hz\nBins: \(freqs.count)")
      let name = (path as NSString).lastPathComponent
      status = "Exported \(name) (\(freqs.count) bins)."
      return true
    } catch {
      status = "FRD export failed: \(error)"
      return false
    }
  }

  func clearCalibration() {
    self.calibration = nil
    self.calibrationPath = nil
    recomputeAverage()
    status = "Calibration cleared."
  }

  /// Apply a calibration curve to a measured magnitude array on the
  /// analysis grid. The mic's response is subtracted in dB, so the
  /// returned array represents the system under test only.
  static func applyCalibration(
    _ raw: [Double], grid: [Double], calibration: CalibrationCurve?
  ) -> [Double] {
    guard let cal = calibration, raw.count == grid.count else { return raw }
    return zip(raw, grid).map { m, f in m - cal.magnitude(at: f) }
  }

  /// Centre a magnitude curve on its in-band median so absolute-level
  /// differences (sweep deconvolution scaling, calibration offsets,
  /// preamp levels) don't drive the auto-fitter into clamping every
  /// band against the gain cap. The fit cares about response shape
  /// against the target curve, not absolute level.
  static func levelNormalize(
    _ magDB: [Double], grid: [Double]
  ) -> [Double] {
    guard magDB.count == grid.count, !magDB.isEmpty else { return magDB }
    var inBand: [Double] = []
    inBand.reserveCapacity(magDB.count)
    for i in 0..<magDB.count where grid[i] >= 200 && grid[i] <= 5000 {
      let v = magDB[i]
      if v.isFinite, v > -200 { inBand.append(v) }
    }
    if inBand.isEmpty { return magDB }
    inBand.sort()
    let median = inBand[inBand.count / 2]
    return magDB.map { $0 - median }
  }

  /// Reset everything to the empty state — useful when switching
  /// between mock and real measurement modes.
  func reset() {
    measuredIR = nil
    measuredFR = nil
    measuredMagDB = []
    grid = []
    correctionPreset = nil
    status = "No measurement loaded."
  }
}
