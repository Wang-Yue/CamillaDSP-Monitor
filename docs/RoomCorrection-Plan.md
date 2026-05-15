# Room Correction — Phased Implementation Plan

## Status overview

The full originally-planned scope is now landed. The DSP core (FFT,
sweep generation/deconvolution, IR/FR analysis, parametric-EQ
auto-fit, FIR design across three modes), the sidebar/preset model
(EQ + Convolution), the engine wiring (`Conv` filter w/ per-rate IR
selection), the editing UI (drag-to-edit EQ with measured/target/
corrected overlays in both Magnitude and Phase tabs), and the
end-to-end mic-capture path (input + output device pickers, sweep
playback, cross-correlation alignment, multi-position averaging,
calibration) are all functional. The Phase-3 polish set —
subwoofer crossover assistant, modal-region EQ constraints, and
mixed-phase FIR — has also landed. What's left is real-hardware
validation across a variety of mics and DACs.

## Building-block inventory

| Capability | Where | State |
|---|---|---|
| Real FFT (vDSP-tuned, arbitrary length) | `CamillaDSPLib/FFT/RealFFT.swift` | ✓ |
| Window functions (BH², Harris, Tukey) | `CamillaDSPLib/FFT/WindowFunction.swift` | ✓ |
| Spectrum analyser | `CamillaDSPLib/Audio/SpectrumAnalyzer.swift` | ✓ |
| Biquad library + public `gainDB` / `phaseRad` | `CamillaDSPLib/BiquadCoefficients.swift` | ✓ |
| EQ preset model + diagram editor (drag/scroll) | `CamillaDSPMonitor/Models/EQPreset.swift`, `Views/EQ*.swift` | ✓ |
| FIR convolution filter (uniform partitioned, zero-alloc) | `CamillaDSPLib/Filters/Convolution.swift` | ✓ |
| Log-sine sweep + Farina inverse | `CamillaDSPLib/Measurement/SweepGenerator.swift` | ✓ |
| One-shot deconvolution → `ImpulseResponse` | `CamillaDSPLib/Measurement/SweepDeconvolver.swift` | ✓ |
| `ImpulseResponse` (peak/centre/Tukey-window) + `FrequencyResponse` (mag/phase/group-delay) | `CamillaDSPLib/Measurement/{ImpulseResponse,FrequencyResponse}.swift` | ✓ |
| Target curve + presets (Flat / B&K / Harman) | `CamillaDSPLib/Measurement/TargetCurve.swift` | ✓ |
| PEQ auto-fit (greedy seed + golden-section coordinate descent) | `CamillaDSPLib/Measurement/PEQAutoFit.swift` | ✓ |
| FIR design — Min-phase / Linear-phase / Measurement-driven | `CamillaDSPLib/Measurement/FIRDesign.swift` | ✓ |
| `ConvolutionPreset` (multi-rate IR family, renameable, persisted) | `CamillaDSPMonitor/Models/ConvolutionPreset.swift` | ✓ |
| Convolution pipeline stage (Same/Separate L/R, per-rate IR selection) | `CamillaDSPMonitor/Models/PipelineStage*.swift` | ✓ |
| Measurement view (4 panes, EQ-diagram-with-overlays magnitude pane) | `CamillaDSPMonitor/Views/MeasurementView.swift` | ✓ |
| Mic + speakers device pickers, AVAudioEngine play+record | `CamillaDSPLib/Backend/MicrophoneDiscovery.swift` (`OutputDeviceDiscovery`), `CamillaDSPLib/Measurement/SweepRecorder.swift` | ✓ |
| Calibration file loader (REW `.frd`, miniDSP UMIK-1/2, generic 2/3-col text) | `CamillaDSPLib/Measurement/CalibrationFile.swift` | ✓ |
| Multi-position spatial averaging | `CamillaDSPMonitor/Models/MeasurementSession.swift` (`recomputeAverage`) | ✓ |
| Fractional-octave smoothing for FR display | `CamillaDSPLib/Measurement/PEQAutoFit.swift` (`smoothLogOctave`) + `MeasurementSession.displaySmoothing` | ✓ |
| Subwoofer crossover assistant | `CamillaDSPLib/Measurement/SubwooferAssist.swift` | ✓ |
| Modal-region EQ constraints (cuts only, high-Q, no low-shelf below Schroeder) | `CamillaDSPLib/Measurement/PEQAutoFit.swift` (`modalMode`, `schroederHz`, `modalMinQ`) | ✓ |
| Mixed-phase FIR (min↔linear blend slider) | `CamillaDSPLib/Measurement/FIRDesign.swift` (`fromMeasurement` + `phaseBlend`) | ✓ |

## Phase 0 — `ConvolutionFilter`

- [x] Port `camilladsp/src/filters/fftconv.rs` → `Convolution.swift`. Uniform-partition overlap-save through `RealFFT`, zero-alloc hot path verified by `HotPathAllocationTests.Convolution_AllocationFree`.
- [x] `FilterType.conv` + `ConvParameters` (Values / Wav / Raw / Dummy, capitalized to match the Rust upstream wire format).
- [x] Coefficient loaders (WAV 16/24/32f/64f, Raw F32/F64/S16/S32 LE, TEXT). Off-thread.
- [x] Migrated user's CamillaDSP-Swift `ConvolutionTests` to swift-testing.

## Phase 1 — Measurement DSP core

- [x] **Sweep gen + Farina inverse** (`SweepGenerator.swift`). Exponential sweep `f1 → f2`, raised-cosine taper, analytic time-reversed inverse with `e^(−R·t)` envelope.
- [x] **One-shot deconvolution** (`SweepDeconvolver.swift`). Single-pass FFT-domain convolution → `ImpulseResponse` centred on the located peak. Round-trip tests cover identity / pure-delay / two-tap MA / in-band flatness.
- [x] **IR + FR types** (`ImpulseResponse.swift` + `FrequencyResponse.swift`). Tukey windowing around the peak, FFT to FR, mag (linear + dB), wrapped + unwrapped phase, group delay via centred difference.
- [x] **Target curve** (`TargetCurve.swift`). Log-frequency piecewise-linear, Codable, three presets.
- [x] **PEQ auto-fit** (`PEQAutoFit.swift`). Three phases:
  - **Seed** — greedy peakings + optional endpoint shelves.
  - **Coordinate descent** — golden-section search over `freq` (log-space, ±1 octave), `gain`, and `Q` per band, holding the rest of the chain fixed. Up to 8 passes; early termination when no parameter moves more than 0.1%.
  - **Cleanup** — drop bands with `|gain| < 0.5 dB`.
  - Replaces the original single-pass greedy implementation; produces visibly different fits for different inputs because it actually finds the local minimum given each input's residual shape.
- [x] **Measurement plot views** (`MeasurementView.swift`). Four panes: Magnitude, Phase, IR, Group Delay.
  - Magnitude pane embeds the existing **`EQDiagramMode`** (preamp slider + draggable bands + footer band-list bar) with a new `EQReferenceOverlay` parameter that draws measured (blue), target (gray dashed), and predicted-corrected (orange) curves *beneath* the editable EQ.
  - Phase pane shows measured (blue) + predicted-corrected (orange = `wrap(measured + EQ_phase)`).
  - Mock measurement randomises the synthetic system per click so each fit exercises a different input.
- [x] **Sidebar reorg.** Room Correction sits under "Audio" between Devices and Dashboard. Convolution presets live under their own "Convolution" sidebar section, alongside EQ Presets.

**Remaining (Phase 1):** the microphone capture path. Sweep playback + recording orchestration (`SweepCapture`) is a thin shim once that exists.

## Phase 2 — FIR convolution

- [x] **`FIRDesign.minimumPhase`** — cepstral min-phase construction from biquad chain. IIR-equivalent magnitude, min-phase pairing, ~0 group delay.
- [x] **`FIRDesign.linearPhase`** — windowed-IFFT with linear-phase factor. Same magnitude as the EQ chain, constant group delay = `taps/2`.
- [x] **`FIRDesign.fromMeasurement`** — designs the IR directly from the *complex* measured FR: `H_corr(f) = target(f) / measured(f)`. Inverts both magnitude AND phase. Bypasses the EQ chain. Constant group delay = `taps/2`. Cosine-tapered toward unity at the band edges. **The only mode that corrects excess phase**; the right choice when corrected-phase still wraps in the Phase tab.
- [x] **Multi-rate IR generation.** `MeasurementSession.generateFIR` designs at every standard sample rate ≥ 32 kHz. `ConvolutionPreset.irPaths` is `[Int: String]` keyed by rate; engine looks up the matching IR at config-build time and falls back to the closest available by log-distance.
- [x] **Pipeline integration.** `StageType.convolution` w/ `ConvChannelMode.same|.separate`, per-channel preset pickers, summary cards showing the IR mini-plot at the live rate. Engine emits `Conv` filters with `type: Raw, format: F64_LE` (matches Rust upstream's `FileSampleFormat::F64_LE`).
- [x] **Convolution preset detail view.** Editable name, per-rate preview picker, file list with Reveal-in-Finder.

## Phase 3 — Refinements

- [x] **Multi-position averaging.** `MeasurementPosition` array on `MeasurementSession`; RMS-magnitude averaging across enabled positions in `recomputeAverage`; UI position-chip strip with toggle / remove and Mock / Import-FRD entry points.
- [x] **Calibration file loader (REW FRD / miniDSP UMIK-1/2 / generic two- or three-column text).** `CalibrationFile.swift`. Auto-fitter sees calibrated magnitudes; export menu offers a separate calibrated-FRD variant.
- [x] **Export REW-compatible `.frd`.** `CalibrationCurve.writeFRD` + `MeasurementSession.exportFRD`. UI menu item, with an optional calibrated variant when a calibration is loaded.
- [x] **Fractional-octave smoothing (1/3, 1/6, 1/12, 1/24) for the magnitude display.** Public `PEQAutoFit.smoothLogOctave`; `MeasurementSession.DisplaySmoothing` + `displayedMagDB`; UI picker in the control bar. The fitter has its own internal AutoEQ-style smoothing and is unaffected.
- [x] **Subwoofer crossover assist** (`SubwooferAssist.swift`). Tags positions as Mains / Subwoofer / Full-range; once at least one of each is captured, derives a crossover recommendation by cross-correlating IRs for time-of-flight delay and walking the magnitude responses for the −6 dB-from-mid-band crossover frequency. Snaps to common values (40/50/60/70/80/90/100/120/150/180/200 Hz). Returns delay (ms), Linkwitz-Riley HP/LP biquads, confidence, and a plain-text summary surfaced in `SubwooferAssistPanel`.
- [x] **Modal-region EQ assistant** (`PEQAutoFit` options `modalMode` / `schroederHz` / `modalMinQ`). Below the Schroeder frequency: cuts only (boosts can't fill modal nulls), Q ≥ 2.0, no low-shelf placement. Toggled from the measurement header.
- [x] **Mixed-phase FIR** (`FIRDesign.fromMeasurement` `phaseBlend`). 0 → minimum-phase (no pre-ring, ~0 latency, magnitude-only); 1 → linear-phase (full mag+phase correction, taps/2 latency, pre-ring); intermediate values blend in the cepstral domain. Surfaced as a 5-step picker (Min-phase / 25 % / 50 % / 75 % / Linear-phase) inside the FIR options menu.

## High-value remaining items

| # | Item | Status |
|---|---|---|
| 1 | **Mic input AudioUnit path** — input device discovery + binding via `kAudioOutputUnitProperty_CurrentDevice` on AVAudioEngine's input node. | ✅ landed (`MicrophoneDiscovery.swift` + UI mic picker; AVAudioEngine inside `SweepRecorder` handles simultaneous I/O without aggregate-device gymnastics — needs real-hardware validation). |
| 2 | **Sweep-play + capture orchestration** — `SweepRecorder` plays the sweep through the selected output while the input node taps the selected mic; cross-correlation against the matched Farina inverse aligns playback latency. | ✅ landed (`SweepRecorder.swift` + `MeasurementSession.captureMeasurement(append:)`; UI menu has "New Capture" / "Add Capture as Position"; status bar surfaces peak level + estimated round-trip latency; output device picker added via `OutputDeviceDiscovery` + `selectedOutputName`). |
| 3 | **Calibration file loader (REW / UMIK)** — magnitude-domain subtraction in the analysis pipeline. | ✅ landed (`CalibrationFile.swift`, supports REW FRD, miniDSP UMIK-1/2, generic 2- or 3-column text). |
| 4 | **Multi-position averaging** — capture multiple positions, RMS-average their magnitudes. | ✅ landed (`MeasurementPosition` + `recomputeAverage`; UI position-chip strip with toggle/remove; FRD-import-as-position). |
| 5 | **Fractional-octave display smoothing** — 1/3, 1/6, 1/12, 1/24 picker. | ✅ landed (`PEQAutoFit.smoothLogOctave` exposed publicly; `MeasurementSession.displaySmoothing` + `displayedMagDB`; UI picker in the control bar). |
| 6 | **Mixed-phase FIR option** — pre/post-ring trade-off slider on top of `fromMeasurement`. | ✅ landed (`FIRDesign.MeasurementOptions.phaseBlend` 0…1; UI surfaced as a 5-step picker inside the FIR options menu when "From measurement" is selected). |
| 7 | ~~Latency-aware DoP / bit-perfect handling~~ | _not needed: DoP→PCM is automatic upstream_ |
| 8 | **REW `.frd` export** — measurement export for external validation. | ✅ landed (`CalibrationCurve.writeFRD` + `MeasurementSession.exportFRD`; UI menu item with optional calibration application). |
| 9 | **Subwoofer crossover assist** — derive delay + crossover from a mains-only and sub-only measurement at the same listening position. | ✅ landed (`SubwooferAssist.swift`; per-position `MeasurementChannelKind` selector; `SubwooferAssistPanel` shows recommended delay, crossover, HP/LP biquads, confidence, and rationale once both kinds are captured). |
| 10 | **Modal-region EQ assistant** — constrain bands below the Schroeder frequency to cuts-only / high-Q / no shelf. | ✅ landed (`PEQAutoFit` options + `MeasurementSession.modalMode`; "Modal" toggle in the control bar). |

Everything in the high-value list is now wired end-to-end. Real-hardware validation is the remaining work — exercising the play+record path against a variety of mics and DACs to confirm latency alignment, format conversion, clipping detection, and output-device binding all behave as expected.

## Proposed enhancements

Quality-of-correction features that aren't in the original scope but would
move this from "good consumer tool" closer to REW / Acourate territory.
Listed in rough priority order — all useful, none blocking what we have.

### ~~1. Frequency-dependent windowing (FDW)~~

✅ **Completed.** Implemented discrete bin-by-bin Frequency-Dependent Windowing using a variable-width Hann window ($T = \text{cycles} / f$). Exposed via `FrequencyResponse.fdw` and a cycle selection control menu in `MeasurementView`. Fully integrated into `MeasurementSession` spatial averaging.

### 2. Per-channel stereo correction workflow (Removed)

❌ **Removed.** Feature removed entirely per user request to maintain a simplified single-channel measurement and export architecture.

### ~~3. Waterfall / cumulative spectral decay (CSD) panel~~

✅ **Completed.** Implemented multi-threaded sliding-window STFT calculations generating time-frequency slices. Rendered via an isometric layered canvas view with depth occlusion. Extended `ImpulseResponse` with `schroederDecay()` and `rt60()` for modal decay time analysis. Verified with comprehensive ground-truth unit tests.

### Notable bug fixes during the AutoEQ pass

- **Sign bug in `accumulateBandResponse`** — the residual update was subtracting the band's response instead of adding it, so each iteration drove the residual *away* from zero. Every fit ended up stacking near-identical bands at the same peak frequency. Tests now lock in the corrected behaviour.
- **Level normalization in `MeasurementSession.levelNormalize`** — deconvolved sweeps produce magnitudes in the 60–90 dB absolute range. Without centring on the in-band median, every fitted band saturated against the gain cap. The fit now sees response *shape*, not absolute level.
- **AutoEQ-style fractional-octave smoothing inside `PEQAutoFit.fit`** (1/12 oct mid, sigmoid-blend to 2 oct above 8 kHz) — dampens noise that was driving inconsistent peak detection across runs.
- **Shelf Q tightened to `[0.4, 0.7]`** (matches AutoEQ) — prevents shelf overshoot from creating fake peaks for the next iteration to chase.

## Risks & open questions (carried from earlier)

- **Simultaneous I/O on macOS.** Aggregate device requires consent + disconnect listener. Investigate avoiding it via `kAudioOutputUnitProperty_StartTimestampsAtZero` + cross-stream timestamps.
- **Round-trip latency calibration.** Auto-detect via cross-correlation peak in the deconvolved IR (default), with a loopback-cable manual override.
- **FIR length vs latency.** Surfaced in the status bar today. 32k-tap linear-phase = ~340 ms latency at 48 kHz; bad for video sync. May want a per-route latency profile.
- ~~Bit-perfect playback.~~ — _DoP is converted to PCM on the fly upstream, so the Convolution stage runs against PCM regardless._
