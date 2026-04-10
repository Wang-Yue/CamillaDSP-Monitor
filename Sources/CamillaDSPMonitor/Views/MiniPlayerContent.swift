// MiniPlayerContent - Mini player content views: spectrum, pipeline, meters

import SwiftUI

// MARK: - Mini Spectrum

struct MiniSpectrumView: View {
  let bands: [Double]

  private static let barGradient = Gradient(stops: [
    .init(color: .green, location: 0.0),
    .init(color: .green, location: 0.35),
    .init(color: .yellow, location: 0.55),
    .init(color: .orange, location: 0.75),
    .init(color: .red, location: 0.95),
    .init(color: .red, location: 1.0),
  ])

  var body: some View {
    Canvas { context, size in
      let count = min(bands.count, 30)
      guard count > 0 else { return }
      let spacing: CGFloat = 1.5
      let totalSpacing = spacing * CGFloat(count - 1)
      let barWidth = max(2, (size.width - totalSpacing) / CGFloat(count))
      let maxHeight = size.height

      for i in 0..<count {
        let x = CGFloat(i) * (barWidth + spacing)
        let normalized = normalizedDB(bands[i])
        let barHeight = max(1, maxHeight * normalized)
        let y = maxHeight - barHeight

        let barRect = CGRect(x: x, y: y, width: barWidth, height: barHeight)
        context.fill(
          Path(roundedRect: barRect, cornerRadius: 1),
          with: .linearGradient(
            Self.barGradient, startPoint: CGPoint(x: x, y: maxHeight),
            endPoint: CGPoint(x: x, y: 0)))
      }
    }
    .frame(height: 60)
  }
}

// MARK: - Mini Pipeline

struct MiniPipelineView: View {
  @EnvironmentObject var appState: AppState

  var body: some View {
    HorizontalScrollWithVerticalWheel {
      HStack(spacing: 3) {
        // Resampler chip
        Button {
          appState.resamplerEnabled.toggle()
        } label: {
          MiniChip(
            icon: "arrow.triangle.2.circlepath",
            label: "Resampler",
            isEnabled: appState.resamplerEnabled
          )
        }
        .buttonStyle(.plain)

        // Pipeline stages — each in its own view so @ObservedObject triggers redraws
        ForEach(appState.stages.indices, id: \.self) { index in
          MiniStageChipButton(stage: appState.stages[index])
        }
      }
    }
    .frame(height: 60)
  }
}

struct MiniStageChipButton: View {
  @ObservedObject var stage: PipelineStage
  @EnvironmentObject var appState: AppState

  var body: some View {
    Button {
      stage.isEnabled.toggle()
      appState.applyConfig()
    } label: {
      MiniChip(
        icon: stage.type.icon,
        label: stage.name,
        isEnabled: stage.isEnabled
      )
    }
    .buttonStyle(.plain)
  }
}

struct MiniChip: View {
  let icon: String
  let label: String
  let isEnabled: Bool

  var body: some View {
    HStack(spacing: 3) {
      Image(systemName: icon)
        .font(.system(size: 8))
      Text(label)
        .font(.system(size: 9, weight: isEnabled ? .semibold : .regular))
        .lineLimit(1)
    }
    .padding(.horizontal, 6)
    .padding(.vertical, 4)
    .background(
      Capsule()
        .fill(isEnabled ? Color.green : Color.gray.opacity(0.3))
    )
    .foregroundStyle(isEnabled ? .black : .white.opacity(0.6))
  }
}

// MARK: - Mini Meters

struct MiniMetersView: View {
  @EnvironmentObject var levels: LevelState

  var body: some View {
    VStack(spacing: 6) {
      MiniMeterRow(label: "L", peak: levels.playbackPeak.left, rms: levels.playbackRms.left)
      MiniMeterRow(label: "R", peak: levels.playbackPeak.right, rms: levels.playbackRms.right)
    }
    .frame(height: 60)
  }
}

struct MiniMeterRow: View {
  let label: String
  let peak: Double
  let rms: Double

  var body: some View {
    HStack(spacing: 6) {
      Text(label)
        .font(.system(size: 10, weight: .medium, design: .monospaced))
        .foregroundStyle(.white.opacity(0.5))
        .frame(width: 12)

      GeometryReader { geo in
        let w = geo.size.width
        let h = geo.size.height

        ZStack(alignment: .leading) {
          // Background
          RoundedRectangle(cornerRadius: 2)
            .fill(Color.white.opacity(0.08))

          // RMS bar (top half)
          RoundedRectangle(cornerRadius: 1.5)
            .fill(
              LinearGradient(
                colors: [.green, .yellow, .orange, .red],
                startPoint: .leading,
                endPoint: .trailing
              )
            )
            .frame(width: w * normalizedDB(rms), height: h / 2 - 0.5)
            .offset(y: -(h / 4))

          // Peak bar (bottom half, dimmer)
          RoundedRectangle(cornerRadius: 1.5)
            .fill(
              LinearGradient(
                colors: [
                  .green.opacity(0.6), .yellow.opacity(0.6), .orange.opacity(0.6),
                  .red.opacity(0.6),
                ],
                startPoint: .leading,
                endPoint: .trailing
              )
            )
            .frame(width: w * normalizedDB(peak), height: h / 2 - 0.5)
            .offset(y: h / 4)
        }
      }

      // dB values
      VStack(alignment: .trailing, spacing: 0) {
        Text(String(format: "%5.1f", rms))
          .font(.system(size: 9, design: .monospaced))
          .foregroundStyle(.white.opacity(0.7))
        Text(String(format: "%5.1f", peak))
          .font(.system(size: 9, design: .monospaced))
          .foregroundStyle(.white.opacity(0.4))
      }
      .frame(width: 38)
    }
  }
}
