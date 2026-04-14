// LevelMeterView - VU-style level meters with peak and RMS

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
/// - Parameters:
///   - maxHeight: drawable height (size.height minus any label gutter)
///   - totalWidth: drawable width (size.width minus any left gutter)
///   - xOffset: left gutter offset (e.g. 20 for dB label column)
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
  let count = min(bands.count, 30)
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
          Path(roundedRect: CGRect(x: 0, y: halfH + 0.5, width: peakW, height: halfH - 1), cornerRadius: r),
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
  @EnvironmentObject var levels: LevelState

  var body: some View {
    HStack(spacing: 16) {
      HStack(spacing: 6) {
        Image(systemName: "mic")
          .font(.caption2)
          .foregroundStyle(.secondary)
        CompactMeterBar(level: levels.capturePeak.left)
        CompactMeterBar(level: levels.capturePeak.right)
      }

      HStack(spacing: 6) {
        Image(systemName: "hifispeaker")
          .font(.caption2)
          .foregroundStyle(.secondary)
        CompactMeterBar(level: levels.playbackPeak.left)
        CompactMeterBar(level: levels.playbackPeak.right)
      }

      Spacer()

      // Status indicator is a separate view so it only redraws when appState
      // changes, not on every 10 Hz meter update.
      CompactStatusIndicator()
    }
  }
}

/// Separated from CompactLevelMeterBar so meter bar redraws (driven by MeterState
/// at 10 Hz) don't also re-evaluate the status indicator (driven by AppState).
private struct CompactStatusIndicator: View {
  @EnvironmentObject var appState: AppState

  var body: some View {
    HStack(spacing: 6) {
      Circle()
        .fill(statusColor)
        .frame(width: 8, height: 8)
      Text(statusLabel)
        .font(.caption2)
        .foregroundStyle(.secondary)
    }
    .help(appState.lastError ?? "")
  }

  private var statusColor: Color {
    switch appState.status {
    case .inactive: return .gray
    case .starting, .applyingConfig: return .yellow
    case .running: return .green
    case .error: return .red
    }
  }

  private var statusLabel: String {
    switch appState.status {
    case .inactive: return "Inactive"
    case .starting: return "Starting..."
    case .running: return "Running"
    case .applyingConfig: return "Updating..."
    case .error: return "Error"
    }
  }
}

struct CompactMeterBar: View {
  let level: Double

  var body: some View {
    Canvas { context, size in
      let bgRect = Path(roundedRect: CGRect(origin: .zero, size: size), cornerRadius: 2)
      context.fill(bgRect, with: .color(Color.primary.opacity(0.06)))

      let barW = size.width * normalizedDB(level)
      if barW > 0 {
        let barRect = CGRect(x: 0, y: 0, width: barW, height: size.height)
        context.fill(
          Path(roundedRect: barRect, cornerRadius: 2), with: .color(level > -6 ? .orange : .green))
      }
    }
    .frame(width: 80, height: 6)
  }
}
