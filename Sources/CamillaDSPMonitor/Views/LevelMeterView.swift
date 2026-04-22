// LevelMeterView - VU-style level meters with peak and RMS

import Observation
import SwiftUI

// MARK: - Shared Visual Constants

extension Gradient {
  /// Standard audio level gradient: green → yellow → orange → red.
  /// Used identically by level meters and spectrum bars.
  static let audioLevel = Gradient(stops: [
    .init(color: .green, location: 0.0),
    .init(color: .green, location: 0.35),
    .init(color: .yellow, location: 0.55),
    .init(color: .orange, location: 0.75),
    .init(color: .red, location: 0.95),
    .init(color: .red, location: 1.0),
  ])
}

// MARK: - Shared Canvas Helpers

/// Draw spectrum bars into a Canvas context.
func drawSpectrumBars(
  context: inout GraphicsContext,
  bands: [Double],
  maxHeight: CGFloat,
  totalWidth: CGFloat,
  xOffset: CGFloat = 0,
  spacing: CGFloat = 2,
  minBarWidth: CGFloat = 4,
  minBarHeight: CGFloat = 2,
  cornerRadius: CGFloat = 2
) {
  let count = min(bands.count, SPECTRUM_BAND_COUNT)
  guard count > 0 else { return }
  let totalSpacing = spacing * CGFloat(count - 1)
  let barWidth = max(minBarWidth, (totalWidth - totalSpacing) / CGFloat(count))
  for i in 0..<count {
    let x = xOffset + CGFloat(i) * (barWidth + spacing)
    let barHeight = max(minBarHeight, maxHeight * normalizedDB(bands[i]))
    let barRect = CGRect(x: x, y: maxHeight - barHeight, width: barWidth, height: barHeight)
    context.fill(
      Path(roundedRect: barRect, cornerRadius: cornerRadius),
      with: .linearGradient(
        .audioLevel,
        startPoint: CGPoint(x: x, y: maxHeight),
        endPoint: CGPoint(x: x, y: 0)))
  }
}

// MARK: - Shared Level Meter Canvas

/// Shared Canvas for drawing dual RMS+Peak level bars.
/// Used by DualLevelMeterView (dashboard) and MiniMeterRow (mini player).
struct LevelMeterCanvas: View {
  let peak: Double
  let rms: Double
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

      if rmsW > 0 {
        context.fill(
          Path(roundedRect: CGRect(x: 0, y: 0.5, width: rmsW, height: halfH - 1), cornerRadius: r),
          with: shading)
      }
      if peakW > 0 {
        context.fill(
          Path(
            roundedRect: CGRect(x: 0, y: halfH + 0.5, width: peakW, height: halfH - 1),
            cornerRadius: r),
          with: shading)
      }

      var divider = Path()
      divider.move(to: CGPoint(x: 0, y: halfH))
      divider.addLine(to: CGPoint(x: w, y: halfH))
      context.stroke(
        divider,
        with: .color(compact ? Color.white.opacity(0.1) : Color.primary.opacity(0.08)),
        lineWidth: 0.5)

      if !compact {
        for dbMark in [-48, -36, -24, -12, -6, -3, 0] {
          let pos = w * normalizedDB(Double(dbMark))
          let markH = dbMark == 0 ? h : h * 0.5
          let markY = dbMark == 0 ? 0 : (h - markH) / 2
          var markPath = Path()
          markPath.move(to: CGPoint(x: pos, y: markY))
          markPath.addLine(to: CGPoint(x: pos, y: markY + markH))
          context.stroke(markPath, with: .color(Color.primary.opacity(0.2)), lineWidth: 1)
        }
      }
    }
  }
}

// MARK: - Dual Peak/RMS Level Meter

struct DualLevelMeterView: View {
  let label: String
  let peak: Double  // dB
  let rms: Double  // dB

  var body: some View {
    HStack(spacing: 8) {
      Text(label)
        .font(.system(.caption, design: .monospaced))
        .foregroundStyle(.secondary)
        .frame(width: 14)

      LevelMeterCanvas(peak: peak, rms: rms)
        .frame(height: 18)

      // dB values: RMS on top, Peak below
      VStack(alignment: .trailing, spacing: 0) {
        Text(String(format: "%5.1f", rms))
          .font(.system(size: 9, design: .monospaced))
          .foregroundStyle(.secondary)
        Text(String(format: "%5.1f", peak))
          .font(.system(size: 9, design: .monospaced))
          .foregroundStyle(.tertiary)
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
      HStack(spacing: 6) {
        Image(systemName: "mic")
          .font(.caption2)
          .foregroundStyle(.secondary)
        CompactStereoMeter(left: levels.capturePeak.left, right: levels.capturePeak.right)
      }

      HStack(spacing: 6) {
        Image(systemName: "hifispeaker")
          .font(.caption2)
          .foregroundStyle(.secondary)
        CompactStereoMeter(left: levels.playbackPeak.left, right: levels.playbackPeak.right)
      }

      Spacer()
      CompactStatusIndicator()
    }
  }
}

/// A batched renderer for two horizontal level bars, maintaining the original look.
struct CompactStereoMeter: View {
  let left: Double
  let right: Double

  var body: some View {
    Canvas { context, size in
      let barW: CGFloat = 80
      let barH: CGFloat = 6
      let spacing: CGFloat = 6

      drawSingleBar(
        context: &context, rect: CGRect(x: 0, y: 0, width: barW, height: barH), level: left)
      drawSingleBar(
        context: &context, rect: CGRect(x: barW + spacing, y: 0, width: barW, height: barH),
        level: right)
    }
    .frame(width: 80 * 2 + 6, height: 6)
  }

  private func drawSingleBar(context: inout GraphicsContext, rect: CGRect, level: Double) {
    context.fill(
      Path(roundedRect: rect, cornerRadius: 2), with: .color(Color.primary.opacity(0.06)))
    let fillW = rect.width * normalizedDB(level)
    if fillW > 0 {
      let fillRect = CGRect(x: rect.minX, y: rect.minY, width: fillW, height: rect.height)
      context.fill(
        Path(roundedRect: fillRect, cornerRadius: 2), with: .color(level > -6 ? .orange : .green))
    }
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
    }
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
