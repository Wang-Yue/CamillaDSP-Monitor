// LevelMeterView - VU-style level meters with peak and RMS

import SwiftUI

/// Normalize a dB value (-60..0) to 0..1 range for meter display.
func normalizedDB(_ db: Double) -> Double {
  max(0, min(1, (db + 60) / 60))
}

// MARK: - Dual Peak/RMS Level Meter

struct DualLevelMeterView: View {
  let label: String
  let peak: Double  // dB
  let rms: Double  // dB

  private let meterGradient = Gradient(stops: [
    .init(color: .green, location: 0.0),
    .init(color: .green, location: 0.35),
    .init(color: .yellow, location: 0.55),
    .init(color: .orange, location: 0.75),
    .init(color: .red, location: 0.95),
    .init(color: .red, location: 1.0),
  ])

  var body: some View {
    HStack(spacing: 8) {
      Text(label)
        .font(.system(.caption, design: .monospaced))
        .foregroundStyle(.secondary)
        .frame(width: 14)

      Canvas { context, size in
        let w = size.width
        let h = size.height
        let halfH = h / 2

        // Background
        let bgRect = Path(roundedRect: CGRect(origin: .zero, size: size), cornerRadius: 3)
        context.fill(bgRect, with: .color(Color.primary.opacity(0.06)))

        let rmsW = w * normalizedDB(rms)
        let peakW = w * normalizedDB(peak)

        // SHARED SHADING: This defines the absolute color mapping for the entire canvas.
        let unifiedShading = GraphicsContext.Shading.linearGradient(
          meterGradient,
          startPoint: .zero,
          endPoint: CGPoint(x: w, y: 0)
        )

        // RMS bar (top half)
        if rmsW > 0 {
          let rmsRect = CGRect(x: 0, y: 0.5, width: rmsW, height: halfH - 1)
          context.fill(Path(roundedRect: rmsRect, cornerRadius: 2), with: unifiedShading)
        }

        // Peak bar (bottom half)
        if peakW > 0 {
          let peakRect = CGRect(x: 0, y: halfH + 0.5, width: peakW, height: halfH - 1)
          // NO OPACITY DIFFERENCE: Use the exact same shading and opacity as the RMS bar
          // to ensure colors are identical at the same horizontal position.
          context.fill(Path(roundedRect: peakRect, cornerRadius: 2), with: unifiedShading)
        }

        // Divider line
        var divider = Path()
        divider.move(to: CGPoint(x: 0, y: halfH))
        divider.addLine(to: CGPoint(x: w, y: halfH))
        context.stroke(divider, with: .color(Color.primary.opacity(0.08)), lineWidth: 0.5)

        // Scale marks
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
  @EnvironmentObject var meters: MeterState
  @EnvironmentObject var appState: AppState

  var body: some View {
    HStack(spacing: 16) {
      HStack(spacing: 6) {
        Image(systemName: "mic")
          .font(.caption2)
          .foregroundStyle(.secondary)
        CompactMeterBar(level: meters.capturePeak.left)
        CompactMeterBar(level: meters.capturePeak.right)
      }

      HStack(spacing: 6) {
        Image(systemName: "hifispeaker")
          .font(.caption2)
          .foregroundStyle(.secondary)
        CompactMeterBar(level: meters.playbackPeak.left)
        CompactMeterBar(level: meters.playbackPeak.right)
      }

      Spacer()

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
