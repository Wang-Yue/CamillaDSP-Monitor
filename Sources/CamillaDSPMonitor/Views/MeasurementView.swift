// Room-correction measurement view.
//
// Four switchable plot panes (Magnitude / Phase / Impulse Response /
// Group Delay) with overlays for measured / target / corrected, plus
// the controls that drive the underlying `MeasurementSession`.
//
// Drawing follows the same Path / Canvas approach as
// `EQDiagramMode` — no Swift Charts dependency, log-frequency X axis,
// dB Y axis. The four panes share the same axis transforms when they
// can (mag / phase / GD all use log-freq X) so the user can mentally
// stack them.

import AppKit
import DSPBackend
import DSPFilters
import DSPMeasurement
import Observation
import SwiftUI
import UniformTypeIdentifiers

private enum MeasurementPane: String, CaseIterable, Identifiable {
  case magnitude = "Magnitude"
  case phase = "Phase"
  case impulse = "Impulse"
  case groupDelay = "Group Delay"
  case waterfall = "Waterfall (CSD)"
  var id: String { rawValue }
}

struct MeasurementView: View {
  @Environment(MeasurementSession.self) var session
  @Environment(PipelineStore.self) var pipeline
  @State private var pane: MeasurementPane = .magnitude
  /// Selection state for the embedded `EQFrequencyResponseView`. Lives
  /// here (not on the session) because it's purely UI state.
  @State private var selectedBandID: UUID? = nil
  /// Whether the subwoofer assist popover is currently presented.
  /// Kept as view state so the inline panel never competes with the
  /// plot for vertical space.
  @State private var subwooferAssistShown: Bool = false

  var body: some View {
    @Bindable var bindable = session
    VStack(spacing: 0) {
      // Toolbar rows are wrapped in horizontal ScrollViews so a narrow
      // window scrolls the controls instead of pushing the sidebar
      // shut. The Menus inside use `.fixedSize()` to keep their
      // summary labels readable (e.g. "Devices: USB Mic → DAC"), so
      // the row's intrinsic width is wider than the typical narrow
      // detail-pane width — without this scroll wrapper, NSSplitView
      // would prioritise the detail pane and steal sidebar width.
      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 12) {
          Menu {
            Section("Real measurement") {
              Button {
                Task { await session.captureMeasurement(append: false) }
              } label: {
                Label("New Capture", systemImage: "mic.circle")
              }
              .disabled(session.isCapturing)
              Button {
                Task { await session.captureMeasurement(append: true) }
              } label: {
                Label("Add Capture as Position", systemImage: "plus.circle")
              }
              .disabled(session.isCapturing || session.positions.isEmpty)
            }
            Section("Mock") {
              Button {
                session.generateMockMeasurement(append: false)
              } label: {
                Label("New Mock Measurement", systemImage: "waveform.path")
              }
              Button {
                session.generateMockMeasurement(append: true)
              } label: {
                Label("Add Mock Position", systemImage: "plus.circle")
              }
              .disabled(session.positions.isEmpty)
            }
            Section {
              Button {
                chooseImportFRD()
              } label: {
                Label("Import FRD as Position…", systemImage: "square.and.arrow.down")
              }
            }
          } label: {
            if session.isCapturing {
              Label("Capturing…", systemImage: "mic.circle.fill")
            } else {
              Label("Measurement", systemImage: "waveform.path")
            }
          }
          .menuStyle(.button)
          .fixedSize()
          .help(
            """
            Source of the measurement that's plotted and fed to the auto-fitter:
            • New Capture — play a Farina log-sweep through the chosen speaker, record from the chosen mic, deconvolve to an impulse response. Replaces the current measurement.
            • Add Capture as Position — same, but appends to the position list for spatial averaging instead of replacing.
            • New / Add Mock — synthetic measurement (random tilt + a few resonances) for trying the EQ/FIR pipeline without a real mic.
            • Import FRD as Position — load an .frd magnitude file (REW / miniDSP) as if it were a captured position.
            """)

          Menu {
            // Mic → device → channel hierarchy: open Mic to see the
            // device list and the per-mic channel picker grouped
            // together. Same shape for Speakers. Helps users grasp
            // "this channel belongs to this device" rather than four
            // unrelated sections at the same level.
            Menu {
              Section("Device") {
                Picker("Mic", selection: $bindable.selectedMicName) {
                  Text("System Default").tag(String?.none)
                  ForEach(
                    CoreAudioCapabilities.availableDeviceNames(isCapture: true), id: \.self
                  ) { name in
                    Text(name).tag(String?.some(name))
                  }
                }
              }
              Section("Channel") {
                Picker("Mic channel", selection: $bindable.selectedInputChannel) {
                  ForEach(0..<max(1, micChannelCount), id: \.self) { idx in
                    Text("Channel \(idx + 1)").tag(idx)
                  }
                }
              }
            } label: {
              Label("Mic — \(micShortLabel)", systemImage: "mic")
            }
            Menu {
              Section("Device") {
                Picker("Speakers", selection: $bindable.selectedOutputName) {
                  Text("System Default").tag(String?.none)
                  ForEach(
                    CoreAudioCapabilities.availableDeviceNames(isCapture: false), id: \.self
                  ) { name in
                    Text(name).tag(String?.some(name))
                  }
                }
              }
              Section("Channel") {
                Picker("Speaker channel", selection: $bindable.selectedOutputChannel) {
                  Text("All channels").tag(-1)
                  ForEach(0..<max(1, outputChannelCount), id: \.self) { idx in
                    Text(outputChannelLabel(idx, total: outputChannelCount)).tag(idx)
                  }
                }
              }
            } label: {
              Label("Speakers — \(speakerShortLabel)", systemImage: "hifispeaker")
            }
          } label: {
            Label(devicesLabel, systemImage: "hifispeaker.and.appletv")
          }
          .menuStyle(.button)
          .fixedSize()
          .help(
            """
            Sweep I/O routing:
            • Mic — device + which capsule (channel) to record from. Pick the calibrated capsule of a stereo / multi-mic interface.
            • Speakers — device + which physical output the sweep plays through. Choose one channel at a time (Left, Right, LFE, …) so the captured response reflects only that speaker; choose "All channels" for stereo / mono playback through every output.
            """)

          Menu {
            Section("Calibration") {
              Button {
                chooseCalibrationFile()
              } label: {
                Label("Load Calibration…", systemImage: "doc.badge.plus")
              }
              if session.calibration != nil {
                Button(role: .destructive) {
                  session.clearCalibration()
                } label: {
                  Label("Clear Calibration", systemImage: "xmark.circle")
                }
              }
            }
            Section("Measurement") {
              Button {
                chooseExportPath(includeCalibration: false)
              } label: {
                Label("Export FRD…", systemImage: "square.and.arrow.up")
              }
              .disabled(session.measuredFR == nil)
              if session.calibration != nil {
                Button {
                  chooseExportPath(includeCalibration: true)
                } label: {
                  Label("Export FRD (calibrated)…", systemImage: "square.and.arrow.up.fill")
                }
                .disabled(session.measuredFR == nil)
              }
            }
          } label: {
            if let path = session.calibrationPath {
              Label((path as NSString).lastPathComponent, systemImage: "checkmark.seal.fill")
            } else {
              Label("File", systemImage: "doc")
            }
          }
          .menuStyle(.button)
          .fixedSize()
          .help(
            """
            Calibration & export:
            • Load Calibration — apply a mic correction file (REW .frd, miniDSP UMIK-1/2, or generic 2- / 3-column text). Subtracted from the measured magnitude so the fitter sees the room, not the mic.
            • Export FRD — save the current measurement as an REW-format .frd. Useful for sanity-checking in REW or sharing.
            """)

          Spacer(minLength: 16)

          Picker("Pane", selection: $pane) {
            ForEach(MeasurementPane.allCases) { p in
              Text(p.rawValue).tag(p)
            }
          }
          .pickerStyle(.segmented)
          .labelsHidden()
          .fixedSize()
          .help(
            """
            Plot pane:
            • Magnitude — frequency response with measured / target / corrected overlays beneath the editable EQ.
            • Phase — wrapped phase of measured + corrected.
            • Impulse — time-domain impulse response (Tukey-windowed around peak).
            • Group Delay — −dφ/dω in milliseconds; reveals all-pass behaviour the magnitude pane hides.
            """)
        }
        .padding(.horizontal)
        .padding(.top, 8)
        .padding(.bottom, 6)
      }

      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 12) {
          Menu {
            Picker("Display smoothing", selection: $bindable.displaySmoothing) {
              ForEach(MeasurementSession.DisplaySmoothing.allCases) { s in
                Text(s.rawValue).tag(s)
              }
            }
          } label: {
            Label("Smoothing: \(session.displaySmoothing.rawValue)", systemImage: "wave.3.right")
          }
          .menuStyle(.button)
          .fixedSize()
          .help(
            """
            Fractional-octave smoothing applied to the displayed curve only — the auto-fitter still sees the unsmoothed response. Wider smoothing (1/3, 1/6) reads modal regions; tighter (1/24) reveals narrow features. "None" shows the raw FFT bins.
            """)

          Menu {
            Picker("FDW", selection: $bindable.fdwCycles) {
              ForEach(MeasurementSession.FDWCycles.allCases) { c in
                Text(c.rawValue).tag(c)
              }
            }
          } label: {
            Label("FDW: \(session.fdwCycles.rawValue)", systemImage: "metronome")
          }
          .menuStyle(.button)
          .fixedSize()
          .help(
            """
            Frequency-dependent windowing applied during analysis. Uses a Hann window whose duration shrinks at higher frequencies (T = cycles / f) to suppress late room reflections while keeping low-frequency mode resolution intact.
            """)

          Menu {
            Picker("Target curve", selection: $bindable.targetPreset) {
              ForEach(TargetCurve.Preset.allCases) { p in
                Text(p.rawValue).tag(p)
              }
            }
          } label: {
            Label("Target: \(session.targetPreset.rawValue)", systemImage: "scope")
          }
          .menuStyle(.button)
          .fixedSize()
          .help(
            """
            What the fitter aims at:
            • Flat — equal-loudness across the band.
            • Brüel & Kjær — gentle high-frequency roll-off matching B&K's in-room target.
            • Harman — Olive's preferred curve: ~+4 dB low-shelf below 100 Hz, gentle treble tilt above 1 kHz.
            """)

          Menu {
            Section {
              Toggle(isOn: $bindable.modalMode) {
                Label("Modal-region constraints", systemImage: "waveform.and.magnifyingglass")
              }
            }
            Section("Schroeder frequency") {
              Picker("Schroeder", selection: $bindable.schroederHz) {
                ForEach([100.0, 150.0, 200.0, 250.0, 300.0, 400.0], id: \.self) { f in
                  Text("\(Int(f)) Hz").tag(f)
                }
              }
            }
            Section("Modal min Q") {
              Picker("Min Q", selection: $bindable.modalMinQ) {
                ForEach([1.5, 2.0, 2.5, 3.0, 4.0], id: \.self) { q in
                  Text(String(format: "%.1f", q)).tag(q)
                }
              }
            }
          } label: {
            Label(modalLabel, systemImage: "waveform.and.magnifyingglass")
          }
          .menuStyle(.button)
          .fixedSize()
          .help(
            """
            Modal-region behaviour below the Schroeder frequency:
            • Modal-region constraints — when on, bands below the Schroeder corner are restricted to negative gain (cuts only — boosts can't fill modal nulls), high Q (≥ Min Q), and no low-shelf placement.
            • Schroeder frequency — boundary between the modal region (individual room modes) and the diffuse field. Use 2000·√(T60/V): roughly 100 Hz for large lossy rooms, 200 Hz for typical living rooms, 300–400 Hz for small treated rooms.
            • Min Q — narrower (higher Q) cuts are more surgical but more sensitive to mic placement. 2.0 is a balanced default.
            """)

          // Generate PEQ doubles as a band-count picker: clicking the
          // primary button uses the current count; the chevron reveals
          // alternative counts that both update the session and run
          // the fit. Replaces the standalone Bands slider that took
          // too much horizontal space.
          Menu {
            Section("Bands to generate") {
              ForEach([3, 5, 8, 10, 12, 16, 20], id: \.self) { count in
                Button {
                  session.bandCount = count
                  session.runFit()
                } label: {
                  if count == session.bandCount {
                    Label("\(count) bands", systemImage: "checkmark")
                  } else {
                    Text("\(count) bands")
                  }
                }
              }
            }
          } label: {
            Label(
              "Generate PEQ (\(session.bandCount))", systemImage: "waveform.badge.magnifyingglass")
          } primaryAction: {
            session.runFit()
          }
          .menuStyle(.button)
          .fixedSize()
          .disabled(session.measuredMagDB.isEmpty)
          .help(
            """
            Run the parametric-EQ auto-fitter:
            • Click — fit the current target with up to the indicated number of bands.
            • Chevron — pick a different band cap (3 / 5 / 8 / 10 / 12 / 16 / 20).
            The fitter seeds with greedy peakings + endpoint shelves, then runs ~8 passes of golden-section coordinate descent over freq / gain / Q. Bands with |gain| < 0.5 dB are dropped.
            """)

          Spacer(minLength: 16)

          Menu {
            Picker("Type", selection: $bindable.firKind) {
              ForEach(FIRKind.allCases) { k in
                Text(k.rawValue).tag(k)
              }
            }
            Picker("Taps", selection: $bindable.firTapCount) {
              Text("2 048").tag(2048)
              Text("4 096").tag(4096)
              Text("8 192").tag(8192)
              Text("16 384").tag(16_384)
              Text("32 768").tag(32_768)
            }
            if session.firKind == .measurementDriven {
              Section("Phase blend") {
                // Sliders inside SwiftUI Menus render as a row; an
                // inline picker over a coarse phase grid is more legible.
                Picker("Phase", selection: phaseBlendBinding) {
                  Text("Min-phase").tag(0)
                  Text("25%").tag(25)
                  Text("50%").tag(50)
                  Text("75%").tag(75)
                  Text("Linear-phase").tag(100)
                }
              }
            }
          } label: {
            Label(firOptionsLabel, systemImage: "slider.horizontal.3")
          }
          .menuStyle(.button)
          .fixedSize()
          .help(
            """
            FIR options:
            • Type — Min-phase (magnitude only, ~0 latency, no pre-ring), Linear-phase (mag + constant group delay = taps/2), From measurement (mag + phase from H = target / measured; the only mode that corrects excess phase).
            • Taps — filter length. 4k–8k handles most rooms; 16k+ for sharp low-frequency correction.
            • Phase blend (measurement mode only) — 0 % = min-phase (no latency, no pre-ring) ↔ 100 % = linear-phase (taps/2 latency, full phase correction).
            """)

          Menu {
            Button {
              applyFitToEQPreset()
            } label: {
              Label(
                "Add as EQ Preset", systemImage: "slider.horizontal.below.square.filled.and.square")
            }
            .disabled(!sessionHasBands)
            Button {
              _ = session.generateFIR(into: pipeline)
            } label: {
              Label("Add as FIR (Convolution) Preset", systemImage: "waveform")
            }
            .disabled(!canGenerateFIR)
          } label: {
            Label("Add to Preset", systemImage: "square.and.arrow.down")
          }
          .menuStyle(.button)
          .fixedSize()
          .disabled(!sessionHasBands && !canGenerateFIR)
          .help(
            """
            Persist the current correction so a pipeline stage can use it:
            • Add as EQ Preset — saves the fitted parametric bands to a new EQ preset (sidebar). An EQ stage in the pipeline can then load it.
            • Add as FIR (Convolution) Preset — designs an impulse response per standard sample rate and saves it as a Convolution preset for a Convolution stage. Uses the FIR options above.
            """)
        }
        .padding(.horizontal)
        .padding(.bottom, 8)
      }

      Divider()

      Group {
        switch pane {
        case .magnitude:
          magnitudePane
        case .phase:
          PhasePlot()
        case .impulse:
          ImpulsePlot()
        case .groupDelay:
          GroupDelayPlot()
        case .waterfall:
          WaterfallPlot()
        }
      }
      .frame(maxWidth: .infinity, maxHeight: .infinity)
      .padding()

      if !session.positions.isEmpty {
        Divider()
        positionsBar
      }
    }
  }

  /// Channel count of the currently-selected mic. Falls back to 2
  /// (stereo) when the device can't be probed — e.g. if the user has
  /// never granted mic permission, the HAL query returns 0.
  /// Two is a sensible default that lets the picker offer a left/right
  /// choice without spuriously forcing channel 1 only.
  private var micChannelCount: Int {
    let n = CoreAudioCapabilities.channelCount(
      deviceName: session.selectedMicName, isCapture: true)
    return n > 0 ? n : 2
  }

  /// Channel count of the currently-selected output device. Same
  /// fallback rationale as `micChannelCount`.
  private var outputChannelCount: Int {
    let n = CoreAudioCapabilities.channelCount(
      deviceName: session.selectedOutputName, isCapture: false)
    return n > 0 ? n : 2
  }

  /// Picker label for an output channel. Adds an L/R hint for stereo
  /// devices and an LFE hint for the conventional 5.1 sub channel
  /// (index 3 in SMPTE order). Other channels just get a plain number.
  private func outputChannelLabel(_ idx: Int, total: Int) -> String {
    if total == 2 {
      switch idx {
      case 0: return "Channel 1 (Left)"
      case 1: return "Channel 2 (Right)"
      default: break
      }
    }
    if total >= 6, idx == 3 {
      return "Channel 4 (LFE)"
    }
    return "Channel \(idx + 1)"
  }

  /// Phase blend stored as 0...1 on the session, but the FIR options
  /// menu picker uses integer percent tags for legibility.
  private var phaseBlendBinding: Binding<Int> {
    Binding(
      get: { Int((session.firPhaseBlend * 100).rounded()) },
      set: { session.firPhaseBlend = Double($0) / 100.0 })
  }

  /// Compact label for the Devices menu showing the current selections
  /// (or "Default") so the user can see what's bound without opening it.
  private var devicesLabel: String {
    let mic = session.selectedMicName.map(shortDeviceName) ?? "Default"
    let out = session.selectedOutputName.map(shortDeviceName) ?? "Default"
    return "Devices: \(mic) → \(out)"
  }

  /// Compact label for the Modal menu showing whether it's active
  /// and the current Schroeder corner. When off, just shows "Modal:
  /// off"; when on, shows the corner so the user can see what
  /// region the constraints are gating without opening the menu.
  private var modalLabel: String {
    if session.modalMode {
      return
        "Modal: \(Int(session.schroederHz)) Hz · Q ≥ \(String(format: "%.1f", session.modalMinQ))"
    }
    return "Modal: off"
  }

  /// Inline label for the "Mic" submenu summarising the bound device
  /// + channel so the parent menu shows current state at a glance.
  private var micShortLabel: String {
    let device = session.selectedMicName.map(shortDeviceName) ?? "Default"
    return "\(device) · ch \(session.selectedInputChannel + 1)"
  }

  /// Inline label for the "Speakers" submenu. Same idea as
  /// `micShortLabel` — surfaces what's currently bound. "All" when
  /// the sweep is fanning out across every output channel.
  private var speakerShortLabel: String {
    let device = session.selectedOutputName.map(shortDeviceName) ?? "Default"
    let ch =
      session.selectedOutputChannel < 0
      ? "all"
      : "ch \(session.selectedOutputChannel + 1)"
    return "\(device) · \(ch)"
  }

  /// Compact label for the FIR options menu summarising the current
  /// kind + taps so the user doesn't have to open it to verify.
  private var firOptionsLabel: String {
    let taps =
      session.firTapCount >= 1024
      ? "\(session.firTapCount / 1024)k"
      : "\(session.firTapCount)"
    return "FIR: \(session.firKind.rawValue) · \(taps)"
  }

  /// Trim long device names so the menu label stays compact. Most
  /// devices have short names; long names usually have a model
  /// number after the brand which we drop.
  private func shortDeviceName(_ name: String) -> String {
    if name.count <= 18 { return name }
    return String(name.prefix(16)) + "…"
  }

  /// True iff there's a correction available to export. Cleaner than
  /// repeating the `correctionPreset?.bands.isEmpty == false` check.
  private var sessionHasBands: Bool {
    (session.correctionPreset?.bands.isEmpty == false)
  }

  /// Open a panel to pick a `.frd` / `.txt` calibration file and
  /// load it into the session.
  private func chooseCalibrationFile() {
    let panel = NSOpenPanel()
    panel.allowsMultipleSelection = false
    panel.canChooseDirectories = false
    panel.canChooseFiles = true
    panel.allowedContentTypes = [.text, .plainText, .data]
    panel.message = """
      Choose a microphone calibration file. Supported formats:
        • REW FRD (.frd)
        • miniDSP UMIK-1 / UMIK-2 (.txt)
        • Two- or three-column text (frequency Hz, magnitude dB, [phase deg])
      """
    if panel.runModal() == .OK, let url = panel.url {
      session.loadCalibration(from: url.path)
    }
  }

  private func chooseImportFRD() {
    let panel = NSOpenPanel()
    panel.allowsMultipleSelection = false
    panel.canChooseDirectories = false
    panel.canChooseFiles = true
    panel.allowedContentTypes = [.text, .plainText, .data]
    panel.message = "Choose an FRD measurement file to add as a position."
    if panel.runModal() == .OK, let url = panel.url {
      session.importPositionFRD(from: url.path)
    }
  }

  /// Bottom strip listing all captured / imported positions. Each
  /// row toggles its position into / out of the average and exposes
  /// rename + delete affordances.
  private var positionsBar: some View {
    HStack(alignment: .top) {
      Text("Positions").font(.caption.bold()).foregroundStyle(.secondary)
        .padding(.top, 6)
      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 8) {
          ForEach(session.positions) { p in
            positionChip(p)
          }
        }
        .padding(.vertical, 6)
      }
      if session.subwooferAssistAvailable {
        Button {
          subwooferAssistShown = true
        } label: {
          Label("Subwoofer Assist", systemImage: "hifispeaker")
        }
        .controlSize(.small)
        .padding(.top, 4)
        .popover(isPresented: $subwooferAssistShown, arrowEdge: .top) {
          SubwooferAssistPanel(session: session)
            .padding()
            .frame(width: 460)
        }
      }
    }
    .padding(.horizontal)
  }

  private func positionChip(_ p: MeasurementPosition) -> some View {
    let kindBinding = Binding<MeasurementChannelKind>(
      get: { p.kind },
      set: { session.setPositionKind(id: p.id, kind: $0) })
    return HStack(spacing: 6) {
      Image(systemName: p.isEnabled ? "checkmark.circle.fill" : "circle")
        .foregroundStyle(p.isEnabled ? Color.accentColor : Color.secondary)
        .onTapGesture { session.togglePosition(id: p.id) }
      Text(p.name)
        .font(.system(.caption, design: .monospaced))
        .foregroundStyle(p.isEnabled ? .primary : .secondary)
      // SwiftUI's `Menu { Picker { ... } }` renders the picker as a
      // submenu trigger, so the user sees an empty top-level menu
      // first. Plain Buttons read as a one-tap chooser instead.
      Menu {
        ForEach(MeasurementChannelKind.allCases) { k in
          Button {
            kindBinding.wrappedValue = k
          } label: {
            if k == p.kind {
              Label(k.rawValue, systemImage: "checkmark")
            } else {
              Text(k.rawValue)
            }
          }
        }
      } label: {
        Text(p.kind.rawValue)
          .font(.system(size: 9, design: .monospaced))
          .foregroundStyle(.secondary)
      }
      .menuStyle(.borderlessButton)
      .fixedSize()
      Button {
        session.removePosition(id: p.id)
      } label: {
        Image(systemName: "xmark.circle.fill")
          .foregroundStyle(.secondary.opacity(0.6))
      }
      .buttonStyle(.plain)
    }
    .padding(.horizontal, 8)
    .padding(.vertical, 4)
    .background(
      RoundedRectangle(cornerRadius: 6)
        .fill(Color.primary.opacity(p.isEnabled ? 0.06 : 0.02))
    )
  }

  /// Save panel for FRD export. The default filename includes the
  /// sample rate so the user gets unique names per measurement.
  private func chooseExportPath(includeCalibration: Bool) {
    let panel = NSSavePanel()
    panel.allowedContentTypes = [.plainText, .data]
    panel.canCreateDirectories = true
    panel.message = "Export the current measurement as an REW-format .frd file."
    let suffix = includeCalibration ? "-calibrated" : ""
    panel.nameFieldStringValue = "Measurement-\(session.sampleRate)Hz\(suffix).frd"
    if panel.runModal() == .OK, let url = panel.url {
      _ = session.exportFRD(to: url.path, includeCalibration: includeCalibration)
    }
  }

  /// Generate-FIR availability depends on which design path is
  /// selected. EQ-derived modes need a non-empty correction;
  /// measurement-driven needs a measurement.
  private var canGenerateFIR: Bool {
    if session.firKind.derivedFromEQ {
      return sessionHasBands
    }
    return session.measuredFR != nil
  }

  /// Magnitude pane contents. Renders through `EQFrequencyResponseView`
  /// so the user gets full drag-to-edit interactivity (drag = freq +
  /// gain, scroll = Q) on the auto-fit result, with the measured
  /// curve, target curve, and predicted post-EQ output as overlays.
  /// Falls back to a simple instructional message when there's no
  /// measurement yet — the EQ view needs a non-empty preset to render
  /// usefully.
  @ViewBuilder
  private var magnitudePane: some View {
    if let preset = session.correctionPreset {
      EQDiagramMode(
        preset: preset,
        selectedBandID: $selectedBandID,
        sampleRate: session.sampleRate,
        overlay: EQReferenceOverlay(
          measuredMagnitudeDB: session.displayedMagDB,
          frequencies: session.grid,
          target: session.targetCurve,
          showCorrected: true)
      )
    } else {
      ContentUnavailableView(
        "No measurement loaded",
        systemImage: "waveform.path",
        description: Text("Click Mock Measurement to generate synthetic data."))
    }
  }

  /// Deep-copy the working `correctionPreset` into `pipeline.eqPresets`
  /// so the user can manage it alongside their other EQs. The
  /// session's own preset stays editable (so they can keep tweaking,
  /// regenerate FIRs, or apply again with a different name).
  private func applyFitToEQPreset() {
    guard let src = session.correctionPreset, !src.bands.isEmpty else { return }
    let copy = EQPreset(
      name: src.name,
      preampGain: src.preampGain,
      bands: src.bands.map { b in
        EQBand(
          type: b.type, freq: b.freq, gain: b.gain, q: b.q,
          isEnabled: b.isEnabled)
      })
    pipeline.eqPresets.append(copy)
    pipeline.saveEQPresets()
    session.status = "Applied as EQ Preset “\(copy.name).” Open it from the sidebar to edit."
  }
}

// MARK: - Shared axis math

private struct LogFreqAxis {
  let minFreq: Double = 20
  let maxFreq: Double = 20_000

  func freqToX(_ f: Double, width: Double) -> Double {
    let logMin = log10(minFreq)
    let logMax = log10(maxFreq)
    return (log10(max(f, minFreq)) - logMin) / (logMax - logMin) * width
  }
}

private struct LinAxis {
  let minVal: Double
  let maxVal: Double
  func toY(_ v: Double, height: Double) -> Double {
    height * (1.0 - (v - minVal) / (maxVal - minVal))
  }
}
// MARK: - Phase plot

private struct PhasePlot: View {
  @Environment(MeasurementSession.self) var session
  @State private var unwrapPhase = false
  private let freqAxis = LogFreqAxis()

  var body: some View {
    GeometryReader { geo in
      let w = geo.size.width
      let h = geo.size.height
      ZStack {
        RoundedRectangle(cornerRadius: 8).fill(Color(nsColor: .textBackgroundColor))
        if let fr = session.measuredFR {
          let unwrapped = unwrapPhase ? fr.unwrappedPhase() : []
          let bounds = phaseBounds(fr: fr, unwrapped: unwrapped)
          gridLines(w: w, h: h, minDeg: bounds.min, maxDeg: bounds.max)
          phasePath(
            fr: fr, unwrapped: unwrapped, minDeg: bounds.min, maxDeg: bounds.max, width: w,
            height: h
          )
          .stroke(Color.blue, lineWidth: 1.2)
          // Predicted post-correction phase = measured + EQ.
          if let preset = session.correctionPreset, !preset.bands.isEmpty {
            correctedPhasePath(
              fr: fr, unwrapped: unwrapped, preset: preset, minDeg: bounds.min, maxDeg: bounds.max,
              width: w, height: h
            )
            .stroke(Color.orange, lineWidth: 1.6)
          }
          phaseLegend

          // Premium floating control at bottom-left
          Button(unwrapPhase ? "Wrap Phase" : "Unwrap Phase") {
            unwrapPhase.toggle()
          }
          .font(.system(size: 11, weight: .medium))
          .controlSize(.small)
          .padding(.horizontal, 8)
          .padding(.vertical, 4)
          .background(.thinMaterial, in: Capsule())
          .padding(8)
          .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomLeading)
        } else {
          gridLines(w: w, h: h, minDeg: -180, maxDeg: 180)
          centerHint("No measurement loaded.")
        }
      }
      .clipShape(RoundedRectangle(cornerRadius: 8))
    }
  }

  private func phaseBounds(fr: FrequencyResponse, unwrapped: [Double]) -> (min: Double, max: Double)
  {
    if !unwrapPhase || unwrapped.isEmpty { return (-180, 180) }
    let step = 1
    var allDegs: [Double] = []
    for k in Swift.stride(from: 1, to: fr.bins, by: step) {
      let f = fr.frequency(at: k)
      if f < 20 || f > 20_000 { continue }
      allDegs.append(unwrapped[k] * 180.0 / .pi)
      if let preset = session.correctionPreset, !preset.bands.isEmpty {
        let cDeg =
          (unwrapped[k] + preset.combinedPhase(atFreq: f, sampleRate: session.sampleRate)) * 180.0
          / .pi
        allDegs.append(cDeg)
      }
    }
    guard let cMin = allDegs.min(), let cMax = allDegs.max() else { return (-180, 180) }
    let span = max(360.0, cMax - cMin)
    let center = (cMax + cMin) / 2.0
    // Give a nice margin
    return (center - span / 2.0 - 45, center + span / 2.0 + 45)
  }

  /// Bottom-right legend so the user can tell which curve is which.
  /// Orange only appears when there's a correction to add.
  private var phaseLegend: some View {
    VStack(alignment: .leading, spacing: 2) {
      legendRow(color: .blue, text: "Measured")
      if let preset = session.correctionPreset, !preset.bands.isEmpty {
        legendRow(color: .orange, text: "Corrected (measured + EQ)")
      }
    }
    .font(.system(size: 10, design: .monospaced))
    .foregroundStyle(.secondary)
    .padding(6)
    .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 4))
    .padding(8)
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)
  }

  private func legendRow(color: Color, text: String) -> some View {
    HStack(spacing: 6) {
      Canvas { context, size in
        var p = Path()
        p.move(to: CGPoint(x: 0, y: size.height / 2))
        p.addLine(to: CGPoint(x: size.width, y: size.height / 2))
        context.stroke(p, with: .color(color), lineWidth: 1.5)
      }
      .frame(width: 18, height: 8)
      Text(text)
    }
  }

  private func gridLines(w: Double, h: Double, minDeg: Double, maxDeg: Double) -> some View {
    ZStack {
      let span = maxDeg - minDeg
      let degStep = span > 2880 ? 1440 : (span > 1440 ? 720 : (span > 720 ? 360 : 90))
      let startDeg = Int(minDeg / Double(degStep)) * degStep
      let endDeg = Int(maxDeg / Double(degStep)) * degStep
      ForEach(Array(Swift.stride(from: startDeg, through: endDeg, by: degStep)), id: \.self) {
        deg in
        let y = h * (1.0 - (Double(deg) - minDeg) / span)
        if y >= 0 && y <= h {
          Path { p in
            p.move(to: CGPoint(x: 0, y: y))
            p.addLine(to: CGPoint(x: w, y: y))
          }.stroke(
            deg == 0 ? Color.primary.opacity(0.18) : Color.primary.opacity(0.06),
            lineWidth: deg == 0 ? 1 : 0.5)
          Text("\(deg)°").font(.system(size: 9, design: .monospaced))
            .foregroundStyle(.tertiary)
            .position(x: 22, y: y - 6)
        }
      }
      ForEach([20, 100, 1000, 10_000], id: \.self) { f in
        let x = freqAxis.freqToX(Double(f), width: w)
        Path { p in
          p.move(to: CGPoint(x: x, y: 0))
          p.addLine(to: CGPoint(x: x, y: h))
        }.stroke(Color.primary.opacity(0.06), lineWidth: 0.5)
        Text(formatFreq(f)).font(.system(size: 9, design: .monospaced))
          .foregroundStyle(.tertiary)
          .position(x: x, y: h - 8)
      }
    }
  }

  private func phasePath(
    fr: FrequencyResponse, unwrapped: [Double], minDeg: Double, maxDeg: Double, width: Double,
    height: Double
  ) -> Path {
    Path { path in
      var started = false
      let step = 1
      let span = maxDeg - minDeg
      for k in Swift.stride(from: 1, to: fr.bins, by: step) {
        let f = fr.frequency(at: k)
        if f < 20 || f > 20_000 { continue }
        let phaseRads = unwrapped.isEmpty ? fr.phase(at: k) : unwrapped[k]
        let phaseDeg = phaseRads * 180.0 / .pi
        let x = freqAxis.freqToX(f, width: width)
        let y = height * (1.0 - (phaseDeg - minDeg) / span)
        if !started {
          path.move(to: CGPoint(x: x, y: y))
          started = true
        } else {
          path.addLine(to: CGPoint(x: x, y: y))
        }
      }
    }
  }

  private func correctedPhasePath(
    fr: FrequencyResponse, unwrapped: [Double], preset: EQPreset, minDeg: Double, maxDeg: Double,
    width: Double, height: Double
  ) -> Path {
    Path { path in
      var started = false
      let step = 1
      let span = maxDeg - minDeg
      for k in Swift.stride(from: 1, to: fr.bins, by: step) {
        let f = fr.frequency(at: k)
        if f < 20 || f > 20_000 { continue }
        let baseRads = unwrapped.isEmpty ? fr.phase(at: k) : unwrapped[k]
        let total = baseRads + preset.combinedPhase(atFreq: f, sampleRate: session.sampleRate)
        let activeRads = unwrapped.isEmpty ? wrapToPi(total) : total
        let phaseDeg = activeRads * 180.0 / .pi
        let x = freqAxis.freqToX(f, width: width)
        let y = height * (1.0 - (phaseDeg - minDeg) / span)
        if !started {
          path.move(to: CGPoint(x: x, y: y))
          started = true
        } else {
          path.addLine(to: CGPoint(x: x, y: y))
        }
      }
    }
  }

  /// Wrap `radians` into `(−π, π]`. The standard idiom; pulled out
  /// here so the inner path-building loop stays readable.
  private func wrapToPi(_ radians: Double) -> Double {
    var r = radians
    while r > .pi { r -= 2 * .pi }
    while r <= -.pi { r += 2 * .pi }
    return r
  }
}

// MARK: - Impulse Response plot

private struct ImpulsePlot: View {
  @Environment(MeasurementSession.self) var session

  var body: some View {
    GeometryReader { geo in
      let w = geo.size.width
      let h = geo.size.height
      ZStack {
        RoundedRectangle(cornerRadius: 8).fill(Color(nsColor: .textBackgroundColor))
        if let ir = session.measuredIR {
          plot(ir: ir, w: w, h: h)
        } else {
          centerHint("No impulse response loaded.")
        }
      }
      .clipShape(RoundedRectangle(cornerRadius: 8))
    }
  }

  private struct IRWindow {
    let lo: Int
    let hi: Int
    let peakAbs: Double
    let halfMs: Double
  }

  private func irWindow(ir: ImpulseResponse) -> IRWindow {
    let halfMs = 50.0
    let halfSamples = Int((halfMs / 1000.0) * Double(ir.sampleRate))
    let lo = max(0, ir.zeroIndex - halfSamples)
    let hi = min(ir.samples.count - 1, ir.zeroIndex + halfSamples)
    var peakAbs = 1e-9
    if lo <= hi {
      for i in lo...hi {
        peakAbs = max(peakAbs, abs(ir.samples[i]))
      }
    }
    return IRWindow(lo: lo, hi: hi, peakAbs: peakAbs, halfMs: halfMs)
  }

  @ViewBuilder
  private func plot(ir: ImpulseResponse, w: Double, h: Double) -> some View {
    let win = irWindow(ir: ir)
    let lo = win.lo
    let hi = win.hi
    let halfMs = win.halfMs
    let lin = LinAxis(minVal: -win.peakAbs * 1.05, maxVal: win.peakAbs * 1.05)

    ZStack {
      // Center line.
      Path { p in
        p.move(to: CGPoint(x: 0, y: h / 2))
        p.addLine(to: CGPoint(x: w, y: h / 2))
      }.stroke(Color.primary.opacity(0.18), lineWidth: 1)

      // Zero-time vertical.
      let xPeak = w * Double(ir.zeroIndex - lo) / Double(hi - lo)
      Path { p in
        p.move(to: CGPoint(x: xPeak, y: 0))
        p.addLine(to: CGPoint(x: xPeak, y: h))
      }.stroke(Color.primary.opacity(0.18), lineWidth: 1)

      // IR samples.
      Path { path in
        for i in lo...hi {
          let x = w * Double(i - lo) / Double(hi - lo)
          let y = lin.toY(ir.samples[i], height: h)
          if i == lo {
            path.move(to: CGPoint(x: x, y: y))
          } else {
            path.addLine(to: CGPoint(x: x, y: y))
          }
        }
      }.stroke(Color.blue, lineWidth: 1.2)

      // Time-axis ticks at -50, -25, 0, +25, +50 ms.
      ForEach([-halfMs, -halfMs / 2, 0, halfMs / 2, halfMs], id: \.self) { ms in
        let sampleOffset = Int((ms / 1000.0) * Double(ir.sampleRate))
        let idx = ir.zeroIndex + sampleOffset
        if idx >= lo && idx <= hi {
          let x = w * Double(idx - lo) / Double(hi - lo)
          Text("\(Int(ms)) ms").font(.system(size: 9, design: .monospaced))
            .foregroundStyle(.tertiary)
            .position(x: x, y: h - 8)
        }
      }
    }
  }
}

// MARK: - Group Delay plot

private struct GroupDelayPlot: View {
  @Environment(MeasurementSession.self) var session
  private let freqAxis = LogFreqAxis()

  var body: some View {
    GeometryReader { geo in
      let w = geo.size.width
      let h = geo.size.height
      ZStack {
        RoundedRectangle(cornerRadius: 8).fill(Color(nsColor: .textBackgroundColor))
        if let fr = session.measuredFR {
          plot(fr: fr, w: w, h: h)
        } else {
          centerHint("No measurement loaded.")
        }
      }
      .clipShape(RoundedRectangle(cornerRadius: 8))
    }
  }

  private func gdAutoScaleMs(fr: FrequencyResponse, gd: [Double]) -> Double {
    var inBand: [Double] = []
    inBand.reserveCapacity(gd.count)
    for k in 1..<fr.bins {
      let f = fr.frequency(at: k)
      if f >= 20 && f <= 20_000 {
        inBand.append(abs(gd[k]))
      }
    }
    inBand.sort()
    let p95 = inBand.isEmpty ? 0.001 : inBand[Int(Double(inBand.count) * 0.95)]
    return max(p95 * 1000.0 * 1.2, 1.0)
  }

  @ViewBuilder
  private func plot(fr: FrequencyResponse, w: Double, h: Double) -> some View {
    let gd = fr.groupDelaySeconds()
    let scaleMs = gdAutoScaleMs(fr: fr, gd: gd)
    let lin = LinAxis(minVal: -scaleMs, maxVal: scaleMs)

    ZStack {
      Path { p in
        p.move(to: CGPoint(x: 0, y: h / 2))
        p.addLine(to: CGPoint(x: w, y: h / 2))
      }.stroke(Color.primary.opacity(0.2), lineWidth: 1)
      Text("0 ms").font(.system(size: 9, design: .monospaced))
        .foregroundStyle(.tertiary)
        .position(x: 28, y: h / 2 - 6)
      Text(String(format: "+%.1f ms", scaleMs))
        .font(.system(size: 9, design: .monospaced))
        .foregroundStyle(.tertiary)
        .position(x: 36, y: 12)
      Text(String(format: "−%.1f ms", scaleMs))
        .font(.system(size: 9, design: .monospaced))
        .foregroundStyle(.tertiary)
        .position(x: 36, y: h - 12)

      Path { path in
        var started = false
        let step = 1
        for k in Swift.stride(from: 1, to: fr.bins, by: step) {
          let f = fr.frequency(at: k)
          if f < 20 || f > 20_000 { continue }
          let x = freqAxis.freqToX(f, width: w)
          let y = lin.toY(gd[k] * 1000.0, height: h)
          if !started {
            path.move(to: CGPoint(x: x, y: y))
            started = true
          } else {
            path.addLine(to: CGPoint(x: x, y: y))
          }
        }
      }.stroke(Color.blue, lineWidth: 1.2)
    }
  }
}

// MARK: - Helpers

private func formatFreq(_ f: Int) -> String {
  if f >= 1000 { return "\(f / 1000)k" }
  return "\(f)"
}

private func centerHint(_ text: String) -> some View {
  Text(text)
    .font(.callout)
    .foregroundStyle(.secondary)
}

// MARK: - Subwoofer Assist panel

/// Inline recommendations for crossover frequency, mains/sub
/// filters, and sub time-of-flight delay. Computed on demand from
/// the most recent `.mains` and `.subwoofer` positions; refreshes
/// when the user clicks Recommend.
struct SubwooferAssistPanel: View {
  @Bindable var session: MeasurementSession
  @State private var recommendation: SubwooferRecommendation?

  var body: some View {
    VStack(alignment: .leading, spacing: 6) {
      HStack {
        Label("Subwoofer Crossover Assist", systemImage: "hifispeaker")
          .font(.subheadline.bold())
        Spacer()
        Button {
          recommendation = session.computeSubwooferRecommendation()
        } label: {
          Label("Recommend", systemImage: "wand.and.stars")
        }
        .controlSize(.small)
      }
      if let r = recommendation {
        HStack(alignment: .top, spacing: 16) {
          metaCell("Crossover", String(format: "%.0f Hz", r.crossoverHz))
          metaCell("Sub delay", String(format: "%+.2f ms", r.subDelayMs))
          metaCell("Mains HP", "\(Int(r.mainsHighPass.freq ?? 0)) Hz · LR2")
          metaCell("Sub LP", "\(Int(r.subLowPass.freq ?? 0)) Hz · LR2")
          metaCell("Confidence", String(format: "%.0f%%", r.confidence * 100))
        }
        Text(r.summary)
          .font(.caption)
          .foregroundStyle(.secondary)
          .fixedSize(horizontal: false, vertical: true)
      } else {
        Text(
          "Click Recommend to compute crossover settings from the most recent mains-only and subwoofer-only measurements."
        )
        .font(.caption)
        .foregroundStyle(.secondary)
      }
    }
  }

  private func metaCell(_ label: String, _ value: String) -> some View {
    VStack(alignment: .leading, spacing: 1) {
      Text(label).font(.caption2).foregroundStyle(.secondary)
      Text(value).font(.system(.caption, design: .monospaced)).bold()
    }
  }
}
