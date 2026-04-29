// EQDiagramMode - Interactive frequency response diagram with draggable band handles

import AppKit
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
        preset: preset, selectedBandID: $selectedBandID, sampleRate: sampleRate
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

struct EQFrequencyResponseView: View {
  let preset: EQPreset
  @Environment(DSPEngineController.self) var dsp
  @Binding var selectedBandID: UUID?
  let sampleRate: Int
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
    GeometryReader { geo in
      let w = geo.size.width
      let h = geo.size.height
      ZStack {
        RoundedRectangle(cornerRadius: 8).fill(Color(nsColor: .textBackgroundColor))
        drawGrid(w: w, h: h)
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
  private func bandHandle(band: EQBand, w: Double, h: Double) -> some View {
    let x = freqToX(band.freq, width: w)
    let gain = band.type.hasGain ? band.gain : 0
    let y = dbToY(gain, height: h)
    let isSelected = band.id == selectedBandID
    let color = colorFor(band)
    return Circle().fill(color).frame(width: isSelected ? 14 : 10, height: isSelected ? 14 : 10)
      .overlay(Circle().stroke(Color.white, lineWidth: isSelected ? 2.5 : 1)).shadow(
        color: .black.opacity(0.3), radius: 2
      ).position(x: x, y: y)
      .gesture(
        DragGesture(minimumDistance: 0).onChanged { value in
          selectedBandID = band.id
          let newFreq = xToFreq(value.location.x, width: w)
          band.freq = max(minFreq, min(maxFreq, newFreq))
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
        Text(String(format: "%.0f Hz", band.freq)).font(.system(size: 8, design: .monospaced))
        if band.type.hasGain {
          Text(String(format: "%+.1f dB", band.gain)).font(.system(size: 8, design: .monospaced))
        }
        if band.type.hasQ {
          Text(String(format: "Q %.2f", band.q)).font(.system(size: 8, design: .monospaced))
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
