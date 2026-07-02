// LevelMeterView - VU-style level meters with peak and RMS

import Observation
import SwiftUI

// MARK: - Shared Visual Constants

/// Returns a color for a given normalized value (0..1) based on the app theme.
/// Mimics the original gradient stops: green (0.35) → yellow (0.55) → orange (0.75) → red (0.95).
func appThemeColor(_ value: Float) -> Color {
  let v = Double(value)
  if v < 0.35 {
    return .green
  } else if v < 0.55 {
    let t = (v - 0.35) / 0.2
    return Color(red: t, green: 1.0, blue: 0)
  } else if v < 0.75 {
    let t = (v - 0.55) / 0.2
    return Color(red: 1.0, green: 1.0 - t * 0.5, blue: 0)
  } else if v < 0.95 {
    let t = (v - 0.75) / 0.2
    return Color(red: 1.0, green: 0.5 - t * 0.5, blue: 0)
  } else {
    return .red
  }
}

private let barOpacity: Double = 0.9

extension Gradient {
  /// Standard audio level gradient: green → yellow → orange → red.
  /// Used identically by level meters and spectrum bars.
  static let audioLevel = Gradient(stops: [
    .init(color: .green.opacity(barOpacity), location: 0.0),
    .init(color: .green.opacity(barOpacity), location: 0.35),
    .init(color: .yellow.opacity(barOpacity), location: 0.55),
    .init(color: .orange.opacity(barOpacity), location: 0.75),
    .init(color: .red.opacity(barOpacity), location: 0.95),
    .init(color: .red.opacity(barOpacity), location: 1.0),
  ])
}

// MARK: - Shared Canvas Helpers

/// Draw spectrum bars into a Canvas context.
func drawSpectrumBars(
  context: inout GraphicsContext,
  bands: [Float],
  maxHeight: CGFloat,
  totalWidth: CGFloat,
  xOffset: CGFloat = 0,
  spacing: CGFloat = 2,
  minBarWidth: CGFloat = 4,
  minBarHeight: CGFloat = 2
) {
  let count = bands.count
  guard count > 0 else { return }
  let totalSpacing = spacing * CGFloat(count - 1)
  let barWidth = max(minBarWidth, (totalWidth - totalSpacing) / CGFloat(count))

  // Resolve the linear gradient shading exactly once for the entire canvas
  let shading = GraphicsContext.Shading.linearGradient(
    .audioLevel,
    startPoint: CGPoint(x: 0, y: maxHeight),
    endPoint: CGPoint(x: 0, y: 0))

  // Batch all bar geometry into a single path using flat rectangles.
  // At typical bar widths (2-4px), rounded corners are visually imperceptible
  // but adding hundreds of vector Bézier curves creates massive CPU overhead.
  // A single low-level `addRects` call achieves maximum CoreGraphics efficiency.
  var path = Path()
  var rects = [CGRect]()
  rects.reserveCapacity(count)

  for i in 0..<count {
    let x = xOffset + CGFloat(i) * (barWidth + spacing)
    let barHeight = max(minBarHeight, maxHeight * normalizedDB(bands[i]))
    rects.append(CGRect(x: x, y: maxHeight - barHeight, width: barWidth, height: barHeight))
  }

  path.addRects(rects)
  context.fill(path, with: shading)
}

// MARK: - Shared Level Meter Canvas

/// Shared Canvas for drawing dual RMS+Peak level bars.
/// Used by DualLevelMeterView (dashboard) and MiniMeterRow (mini player).
struct LevelMeterCanvas: View {
  let peak: Float
  let rms: Float
  /// compact = mini player style: smaller radii, white-based colors, no scale marks
  var compact: Bool = false

  var body: some View {
    Canvas { context, size in
      let w = size.width
      let h = size.height
      let halfH = h / 2

      context.fill(
        Path(roundedRect: CGRect(origin: .zero, size: size), cornerRadius: compact ? 2 : 3),
        with: .color(compact ? Color.white.opacity(0.08) : Color.primary.opacity(0.06)))

      let rmsW = w * normalizedDB(rms)
      let peakW = w * normalizedDB(peak)

      // Shared shading — same color at the same horizontal position for both bars
      let shading = GraphicsContext.Shading.linearGradient(
        .audioLevel, startPoint: .zero, endPoint: CGPoint(x: w, y: 0))
      let r: CGFloat = compact ? 1.5 : 2

      var barsPath = Path()
      let cornerSize = CGSize(width: r, height: r)
      if rmsW > 0 {
        barsPath.addRoundedRect(
          in: CGRect(x: 0, y: 0.5, width: rmsW, height: halfH - 1), cornerSize: cornerSize)
      }
      if peakW > 0 {
        barsPath.addRoundedRect(
          in: CGRect(x: 0, y: halfH + 0.5, width: peakW, height: halfH - 1), cornerSize: cornerSize)
      }
      context.fill(barsPath, with: shading)

      var divider = Path()
      divider.move(to: CGPoint(x: 0, y: halfH))
      divider.addLine(to: CGPoint(x: w, y: halfH))
      context.stroke(
        divider,
        with: .color(compact ? Color.white.opacity(0.1) : Color.primary.opacity(0.08)),
        lineWidth: 0.5)

      if !compact {
        var marksPath = Path()
        for dbMark in [-48, -36, -24, -12, -6, -3, 0] {
          let pos = w * normalizedDB(Float(dbMark))
          let markH = dbMark == 0 ? h : h * 0.5
          let markY = dbMark == 0 ? 0 : (h - markH) / 2
          marksPath.move(to: CGPoint(x: pos, y: markY))
          marksPath.addLine(to: CGPoint(x: pos, y: markY + markH))
        }
        context.stroke(marksPath, with: .color(Color.primary.opacity(0.2)), lineWidth: 1)
      }
    }
  }
}

// MARK: - Dual Peak/RMS Level Meter

struct DualLevelMeterView: View {
  let label: String
  let peak: Float  // dB
  let rms: Float  // dB

  var body: some View {
    HStack(spacing: 8) {
      Text(label)
        .font(.system(.caption, design: .monospaced))
        .foregroundStyle(.secondary)
        .frame(width: 14)
        .fixedSize()

      LevelMeterCanvas(peak: peak, rms: rms)
        .frame(height: 18)

      // dB values: RMS on top, Peak below
      VStack(alignment: .trailing, spacing: 0) {
        Text(String(format: "%5.1f", rms))
          .font(.system(size: 9, design: .monospaced))
          .foregroundStyle(.secondary)
          .fixedSize()
        Text(String(format: "%5.1f", peak))
          .font(.system(size: 9, design: .monospaced))
          .foregroundStyle(.tertiary)
          .fixedSize()
      }
      .frame(width: 44, alignment: .trailing)
    }
  }
}

// MARK: - Compact Level Meter Bar

struct CompactLevelMeterBar: View {
  @Environment(LevelState.self) var levels

  var body: some View {
    HStack(spacing: 16) {
      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 16) {
          HStack(spacing: 6) {
            Image(systemName: "mic")
              .font(.caption2)
              .foregroundStyle(.secondary)
            CompactMultiChannelMeter(levels: levels.capturePeak)
          }

          HStack(spacing: 6) {
            Image(systemName: "hifispeaker")
              .font(.caption2)
              .foregroundStyle(.secondary)
            CompactMultiChannelMeter(levels: levels.playbackPeak)
          }
        }
      }
      .frame(maxWidth: .infinity, alignment: .leading)

      CompactStatusIndicator()
    }
    .onAppear { levels.visibilityCount += 1 }
    .onDisappear { levels.visibilityCount -= 1 }
  }
}

/// A batched renderer for multiple horizontal level bars, maintaining the original look.
struct CompactMultiChannelMeter: View {
  let levels: [Float]

  var body: some View {
    let count = levels.count
    let barW: CGFloat = count > 4 ? 40 : 80
    let spacing: CGFloat = 4
    let totalWidth = count > 0 ? (barW + spacing) * CGFloat(count) - spacing : 0

    Canvas { context, size in
      guard count > 0 else { return }

      for i in 0..<count {
        let x = CGFloat(i) * (barW + spacing)
        let rect = CGRect(x: x, y: 0, width: barW, height: size.height)

        context.fill(
          Path(roundedRect: rect, cornerRadius: 1.5),
          with: .color(Color.primary.opacity(0.06)))

        let fillW = barW * normalizedDB(levels[i])
        if fillW > 0 {
          let fillRect = CGRect(x: rect.minX, y: rect.minY, width: fillW, height: rect.height)
          context.fill(
            Path(roundedRect: fillRect, cornerRadius: 1.5),
            with: .color(appThemeColor(Float(normalizedDB(levels[i])))))
        }
      }
    }
    .frame(width: totalWidth, height: 6)
  }
}

private struct CompactStatusIndicator: View {
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    HStack(spacing: 6) {
      Circle()
        .fill(statusColor)
        .frame(width: 8, height: 8)
      Text(statusLabel)
        .font(.caption2)
        .foregroundStyle(.secondary)
        .fixedSize(horizontal: true, vertical: false)
    }
    .fixedSize()
  }

  private var statusColor: Color {
    switch dsp.status {
    case .inactive: return .gray
    case .starting: return .yellow
    case .running: return .green
    case .paused: return .blue
    case .stalled: return .orange
    }
  }

  private var statusLabel: String {
    switch dsp.status {
    case .inactive: return "Inactive"
    case .starting: return "Starting..."
    case .running: return "Running"
    case .paused: return "Paused"
    case .stalled: return "Stalled"
    }
  }
}
