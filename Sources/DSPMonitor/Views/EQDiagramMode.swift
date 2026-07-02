// EQDiagramMode - Interactive frequency response diagram with draggable band handles

import AppKit
import DSPMeasurement
import Observation
import SwiftUI

// MARK: - Scroll Wheel Monitor (for Q adjustment)

/// Uses NSEvent local monitor to observe scroll wheel events without
/// interfering with SwiftUI's gesture system (clicks, drags).
private struct ScrollWheelMonitor: ViewModifier {
  let action: (CGFloat) -> Void
  @State private var monitor: Any?
  @State private var isHovered = false

  func body(content: Content) -> some View {
    content
      .onHover { hovering in
        isHovered = hovering
        if hovering && monitor == nil {
          monitor = NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) { event in
            guard isHovered else { return event }
            let delta = event.scrollingDeltaY
            if abs(delta) > 0.01 {
              action(delta)
            }
            return event
          }
        } else if !hovering, let m = monitor {
          NSEvent.removeMonitor(m)
          monitor = nil
        }
      }
      .onDisappear {
        if let m = monitor {
          NSEvent.removeMonitor(m)
          monitor = nil
        }
      }
  }
}

extension View {
  func onScrollGesture(action: @escaping (CGFloat) -> Void) -> some View {
    modifier(ScrollWheelMonitor(action: action))
  }
}

struct EQDiagramMode: View {
  @Bindable var preset: EQPreset
  @Binding var selectedBandID: UUID?
  let sampleRate: Int
  @Environment(DSPEngineController.self) var dsp
  /// Optional measurement-context overlay drawn beneath the EQ
  /// curves. Default `nil` keeps EQ Preset Detail's appearance
  /// unchanged; the Room Correction view passes a populated overlay
  /// so measured / target / corrected curves render alongside the
  /// editable EQ.
  var overlay: EQReferenceOverlay? = nil

  var body: some View {
    VStack(spacing: 0) {
      HStack {
        Label("Preamp", systemImage: "speaker.wave.2")
          .font(.caption).foregroundStyle(.secondary)
        Slider(value: $preset.preampGain, in: -20...12, step: 0.1)
          .controlSize(.small)
        Text(String(format: "%+.1f dB", preset.preampGain))
          .font(.system(.caption, design: .monospaced))
          .frame(width: 50, alignment: .trailing)
      }
      .padding(.horizontal)
      .padding(.top, 8)

      EQFrequencyResponseView(
        preset: preset,
        selectedBandID: $selectedBandID,
        sampleRate: sampleRate,
        overlay: overlay
      )
      .frame(minHeight: 300)
      .padding()

      Divider()

      EQBandListBar(preset: preset, selectedBandID: $selectedBandID)
        .padding(.horizontal)
        .padding(.vertical, 8)
    }
  }
}

/// Reference-data overlay drawn beneath the EQ band curves. Used by
/// the Room Correction view to show the measured response (blue),
/// the target curve (gray dashed), and the predicted output =
/// measured + EQ (orange). All three are optional — when `nil`, the
/// view falls back to its original look-and-feel for the EQ Preset
/// Detail use-case.
struct EQReferenceOverlay {
  /// dB magnitudes sampled at `frequencies` (parallel arrays). Both
  /// must be the same length and non-empty for the overlay to draw.
  var measuredMagnitudeDB: [Double] = []
  var frequencies: [Double] = []
  /// Target curve to render as a gray dashed line.
  var target: TargetCurve? = nil
  /// Show predicted post-EQ response = measured + EQ as an orange
  /// curve. Requires `measuredMagnitudeDB` and `frequencies` to be
  /// populated (otherwise the overlay has nothing to add to).
  var showCorrected: Bool = false
}

struct EQFrequencyResponseView: View {
  let preset: EQPreset
  @Environment(DSPEngineController.self) var dsp
  @Binding var selectedBandID: UUID?
  let sampleRate: Int
  /// Optional measurement-context overlay. Default is `nil` so EQ
  /// Preset Detail view's appearance is unchanged.
  var overlay: EQReferenceOverlay? = nil
  static let bandColors: [Color] = [
    .red, .orange, .yellow, .green, .cyan, .blue, .purple, .pink, .mint, .teal, .indigo, .brown,
  ]
  private func colorFor(_ band: EQBand) -> Color {
    guard let idx = preset.bands.firstIndex(where: { $0.id == band.id }) else { return .gray }
    return Self.bandColors[idx % Self.bandColors.count]
  }
  private let minFreq = 20.0, maxFreq = 20000.0, minDB = -24.0, maxDB = 24.0, numPoints = 1000
  private func freqToX(_ f: Double, width: Double) -> Double {
    let logMin = log10(minFreq)
    let logMax = log10(maxFreq)
    return (log10(max(f, minFreq)) - logMin) / (logMax - logMin) * width
  }
  private func xToFreq(_ x: Double, width: Double) -> Double {
    let logMin = log10(minFreq)
    let logMax = log10(maxFreq)
    let logF = logMin + (x / width) * (logMax - logMin)
    return pow(10, logF)
  }
  private func dbToY(_ db: Double, height: Double) -> Double {
    return height * (1.0 - (db - minDB) / (maxDB - minDB))
  }
  private func yToDB(_ y: Double, height: Double) -> Double {
    let ratio = 1.0 - y / height
    return minDB + ratio * (maxDB - minDB)
  }

  var body: some View {
    // Compute overlay reference offset once per body evaluation so
    // the measured/corrected paths share a consistent baseline.
    let normDB = overlayReferenceDB
    return GeometryReader { geo in
      let w = geo.size.width
      let h = geo.size.height
      ZStack {
        RoundedRectangle(cornerRadius: 8).fill(Color(nsColor: .textBackgroundColor))
        drawGrid(w: w, h: h)

        // Reference overlays sit beneath the EQ curves so the user's
        // editable bands always render on top.
        if let target = overlay?.target {
          targetPath(target, w: w, h: h)
            .stroke(Color.secondary, style: StrokeStyle(lineWidth: 1.2, dash: [4, 3]))
        }
        if let ovl = overlay, !ovl.measuredMagnitudeDB.isEmpty,
          ovl.measuredMagnitudeDB.count == ovl.frequencies.count
        {
          measuredPath(ovl, normDB: normDB, w: w, h: h)
            .stroke(Color.blue, lineWidth: 1.4)
          if ovl.showCorrected {
            correctedPath(ovl, normDB: normDB, w: w, h: h)
              .stroke(Color.orange, lineWidth: 1.8)
          }
        }

        ForEach(preset.bands) { band in
          let color = colorFor(band)
          bandCurve(band: band, w: w, h: h).stroke(
            band.id == selectedBandID ? color : color.opacity(0.35),
            lineWidth: band.id == selectedBandID ? 2 : 1)
        }
        combinedCurve(w: w, h: h).stroke(Color.accentColor, lineWidth: 2.5)
        ForEach(preset.bands) { band in bandHandle(band: band, w: w, h: h) }
      }
      .clipShape(RoundedRectangle(cornerRadius: 8))
      .onTapGesture {
        selectedBandID = nil
      }
      .onScrollGesture { delta in
        adjustSelectedBandQ(delta: delta)
      }
    }
  }

  // MARK: - Reference overlay paths

  /// Median in-band magnitude (200 Hz – 5 kHz) used to anchor the
  /// measured / corrected curves at ~0 dB so the user is comparing
  /// shape, not absolute level. Single-bin nulls would otherwise
  /// blow the offset out by tens of dB.
  private var overlayReferenceDB: Double {
    guard let ovl = overlay,
      !ovl.measuredMagnitudeDB.isEmpty,
      ovl.measuredMagnitudeDB.count == ovl.frequencies.count
    else { return 0 }
    var inBand: [Double] = []
    inBand.reserveCapacity(ovl.measuredMagnitudeDB.count)
    for i in 0..<ovl.frequencies.count {
      let f = ovl.frequencies[i]
      let m = ovl.measuredMagnitudeDB[i]
      if f >= 200, f <= 5000, m.isFinite, m > -200 {
        inBand.append(m)
      }
    }
    if inBand.isEmpty { return 0 }
    inBand.sort()
    return inBand[inBand.count / 2]
  }

  private func clampForPlot(_ db: Double) -> Double {
    let lo = minDB - 6
    let hi = maxDB + 6
    if db.isFinite { return max(lo, min(hi, db)) }
    return 0
  }

  private func targetPath(_ target: TargetCurve, w: Double, h: Double) -> Path {
    Path { path in
      let n = 256
      for i in 0...n {
        let x = w * Double(i) / Double(n)
        let f = xToFreq(x, width: w)
        let db = clampForPlot(target.evaluate(atFreqHz: f))
        let y = dbToY(db, height: h)
        if i == 0 {
          path.move(to: CGPoint(x: x, y: y))
        } else {
          path.addLine(to: CGPoint(x: x, y: y))
        }
      }
    }
  }

  private func measuredPath(_ ovl: EQReferenceOverlay, normDB: Double, w: Double, h: Double) -> Path
  {
    Path { path in
      var started = false
      for i in 0..<ovl.frequencies.count {
        let x = freqToX(ovl.frequencies[i], width: w)
        let dB = clampForPlot(ovl.measuredMagnitudeDB[i] - normDB)
        let y = dbToY(dB, height: h)
        if !started {
          path.move(to: CGPoint(x: x, y: y))
          started = true
        } else {
          path.addLine(to: CGPoint(x: x, y: y))
        }
      }
    }
  }

  /// `measured + EQ` — the predicted post-correction response.
  /// `preset.combinedResponse` already includes the preamp gain.
  private func correctedPath(
    _ ovl: EQReferenceOverlay, normDB: Double, w: Double, h: Double
  ) -> Path {
    Path { path in
      var started = false
      for i in 0..<ovl.frequencies.count {
        let f = ovl.frequencies[i]
        let dB = clampForPlot(
          ovl.measuredMagnitudeDB[i] - normDB
            + preset.combinedResponse(atFreq: f, sampleRate: sampleRate))
        let x = freqToX(f, width: w)
        let y = dbToY(dB, height: h)
        if !started {
          path.move(to: CGPoint(x: x, y: y))
          started = true
        } else {
          path.addLine(to: CGPoint(x: x, y: y))
        }
      }
    }
  }

  private func adjustSelectedBandQ(delta: CGFloat) {
    guard let id = selectedBandID,
      let band = preset.bands.first(where: { $0.id == id }),
      band.type.hasQ
    else { return }
    // Scroll up = higher Q (narrower), scroll down = lower Q (wider)
    // Multiplicative scaling so it feels natural at all Q values
    let factor = delta > 0 ? 1.05 : 0.95
    band.q = max(0.1, min(20.0, band.q * factor))
    dsp.applyConfig()
  }

  private func drawGrid(w: Double, h: Double) -> some View {
    ZStack {
      ForEach([-18, -12, -6, 0, 6, 12, 18], id: \.self) { db in
        let y = dbToY(Double(db), height: h)
        Path { p in
          p.move(to: CGPoint(x: 0, y: y))
          p.addLine(to: CGPoint(x: w, y: y))
        }.stroke(
          db == 0 ? Color.primary.opacity(0.2) : Color.primary.opacity(0.06),
          lineWidth: db == 0 ? 1 : 0.5)
        Text("\(db) dB").font(.system(size: 9, design: .monospaced)).foregroundStyle(.tertiary)
          .position(x: 28, y: y - 8)
      }
      ForEach([20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000], id: \.self) { freq in
        let x = freqToX(Double(freq), width: w)
        Path { p in
          p.move(to: CGPoint(x: x, y: 0))
          p.addLine(to: CGPoint(x: x, y: h))
        }.stroke(Color.primary.opacity(0.06), lineWidth: 0.5)
        Text(formatFreq(freq)).font(.system(size: 9, design: .monospaced)).foregroundStyle(
          .tertiary
        ).position(x: x, y: h - 8)
      }
    }
  }
  private func formatFreq(_ f: Int) -> String { f >= 1000 ? "\(f / 1000)k" : "\(f)" }
  private func bandCurve(band: EQBand, w: Double, h: Double) -> Path {
    Path { path in
      guard band.isEnabled else { return }
      for i in 0...numPoints {
        let x = w * Double(i) / Double(numPoints)
        let f = xToFreq(x, width: w)
        let db = band.response(atFreq: f, sampleRate: sampleRate)
        let y = dbToY(db, height: h)
        if i == 0 {
          path.move(to: CGPoint(x: x, y: y))
        } else {
          path.addLine(to: CGPoint(x: x, y: y))
        }
      }
    }
  }
  private func combinedCurve(w: Double, h: Double) -> Path {
    Path { path in
      for i in 0...numPoints {
        let x = w * Double(i) / Double(numPoints)
        let f = xToFreq(x, width: w)
        let db = preset.combinedResponse(atFreq: f, sampleRate: sampleRate)
        let y = dbToY(db, height: h)
        if i == 0 {
          path.move(to: CGPoint(x: x, y: y))
        } else {
          path.addLine(to: CGPoint(x: x, y: y))
        }
      }
    }
  }
  @ViewBuilder
  private func bandHandle(band: EQBand, w: Double, h: Double) -> some View {
    if band.type == .free {
      EmptyView()
    } else {
      let handleFreq: Double = {
        switch band.type {
        case .generalNotch: return band.freqNotch
        case .linkwitzTransform: return band.freqTarget
        default: return band.freq
        }
      }()
      let x = freqToX(handleFreq, width: w)
      let gain = band.type.hasGain ? band.gain : 0
      let y = dbToY(gain, height: h)
      let isSelected = band.id == selectedBandID
      let color = colorFor(band)

      Circle().fill(color).frame(width: isSelected ? 14 : 10, height: isSelected ? 14 : 10)
        .overlay(Circle().stroke(Color.white, lineWidth: isSelected ? 2.5 : 1)).shadow(
          color: .black.opacity(0.3), radius: 2
        ).position(x: x, y: y)
        .gesture(
          DragGesture(minimumDistance: 0).onChanged { value in
            selectedBandID = band.id
            let newFreq = xToFreq(value.location.x, width: w)
            let clampedFreq = max(minFreq, min(maxFreq, newFreq))
            switch band.type {
            case .generalNotch:
              band.freqNotch = clampedFreq
            case .linkwitzTransform:
              band.freqTarget = clampedFreq
            default:
              band.freq = clampedFreq
            }
            if band.type.hasGain {
              let newDB = yToDB(value.location.y, height: h)
              band.gain = max(-20, min(20, (newDB * 2).rounded() / 2))
            }
            dsp.applyConfig()
          }.onEnded { _ in
            dsp.applyConfig()
          }
        )
        .onTapGesture { selectedBandID = band.id }
    }
  }
}

struct EQBandListBar: View {
  let preset: EQPreset
  @Binding var selectedBandID: UUID?
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    HStack {
      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 4) {
          ForEach(Array(preset.bands.enumerated()), id: \.element.id) { i, band in
            let color = EQFrequencyResponseView.bandColors[
              i % EQFrequencyResponseView.bandColors.count]
            EQBandChip(
              preset: preset,
              band: band, index: i + 1, isSelected: band.id == selectedBandID, color: color,
              selectedBandID: $selectedBandID
            ).onTapGesture { selectedBandID = band.id }
          }
        }
      }
      .frame(maxWidth: .infinity)

      HStack(spacing: 12) {
        Button {
          preset.addBand()
          dsp.applyConfig()
        } label: {
          Image(systemName: "plus.circle")
        }.buttonStyle(.plain)
      }
      .padding(.leading, 8)
    }
  }
}

struct EQBandChip: View {
  let preset: EQPreset
  let band: EQBand
  let index: Int
  let isSelected: Bool
  let color: Color
  @Binding var selectedBandID: UUID?
  @Environment(DSPEngineController.self) var dsp
  var body: some View {
    HStack(spacing: 4) {
      Circle().fill(color).frame(width: 6, height: 6)
      VStack(alignment: .leading, spacing: 1) {
        Text("#\(index) \(band.type.rawValue)").font(
          .system(size: 9, weight: isSelected ? .bold : .regular))

        if band.type == .free {
          // Free has no freq/gain/q
        } else {
          let displayFreq: Double = {
            switch band.type {
            case .generalNotch: return band.freqNotch
            case .linkwitzTransform: return band.freqTarget
            default: return band.freq
            }
          }()
          Text(String(format: "%.0f Hz", displayFreq)).font(.system(size: 8, design: .monospaced))

          if band.type.hasGain {
            Text(String(format: "%+.1f dB", band.gain)).font(.system(size: 8, design: .monospaced))
          }

          let displayQ: Double? = {
            if band.type == .generalNotch {
              return band.qPole
            } else if band.type == .linkwitzTransform {
              return band.qTarget
            } else if band.type.hasQ {
              return band.q
            }
            return nil
          }()
          if let qVal = displayQ {
            let label =
              band.type == .generalNotch ? "Qp" : (band.type == .linkwitzTransform ? "Qt" : "Q")
            Text(String(format: "\(label) %.2f", qVal)).font(.system(size: 8, design: .monospaced))
          }
        }
      }
    }.padding(.horizontal, 6).padding(.vertical, 3).background(
      RoundedRectangle(cornerRadius: 6).fill(
        isSelected ? color.opacity(0.15) : Color.primary.opacity(0.04))
    ).overlay(
      RoundedRectangle(cornerRadius: 6).stroke(isSelected ? color : Color.clear, lineWidth: 1)
    ).foregroundStyle(band.isEnabled ? .primary : .tertiary)
      .contextMenu {
        Button {
          band.isEnabled.toggle()
          dsp.applyConfig()
        } label: {
          Label(
            band.isEnabled ? "Disable Band" : "Enable Band",
            systemImage: band.isEnabled ? "eye.slash" : "eye")
        }

        Menu {
          ForEach(EQBandType.allCases) { type in
            Button(type.rawValue) {
              band.type = type
              dsp.applyConfig()
            }
          }
        } label: {
          Label("Change Type", systemImage: "slider.horizontal.3")
        }

        Button(role: .destructive) {
          if let idx = preset.bands.firstIndex(where: { $0.id == band.id }) {
            preset.removeBand(at: idx)
            selectedBandID = nil
            dsp.applyConfig()
          }
        } label: {
          Label("Delete", systemImage: "trash")
        }
      }
  }
}
