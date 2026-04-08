// MiniPlayerContent - Mini player content views: spectrum, pipeline, meters

import SwiftUI

// MARK: - Mini Spectrum

struct MiniSpectrumView: View {
  let bands: [Double]

  var body: some View {
    GeometryReader { geo in
      let barWidth = max(
        2, (geo.size.width - CGFloat(bands.count - 1) * 1.5) / CGFloat(bands.count))
      let maxHeight = geo.size.height

      VStack {
        Spacer(minLength: 0)
        HStack(alignment: .bottom, spacing: 1.5) {
          ForEach(0..<min(bands.count, 30), id: \.self) { i in
            let normalized = normalizedDB(bands[i])
            let height = max(1, maxHeight * normalized)

            RoundedRectangle(cornerRadius: 1)
              .fill(
                LinearGradient(
                  stops: [
                    .init(color: .green, location: 0.0),
                    .init(color: .green, location: 0.35),
                    .init(color: .yellow, location: 0.55),
                    .init(color: .orange, location: 0.75),
                    .init(color: .red, location: 0.95),
                  ],
                  startPoint: .bottom,
                  endPoint: .top
                )
              )
              .frame(width: barWidth, height: height)
          }
        }
      }
    }
    .frame(height: 60)
    .drawingGroup()
  }
}

// MARK: - Mini Pipeline

struct MiniPipelineView: View {
  @EnvironmentObject var appState: AppState

  var body: some View {
    ScrollView(.horizontal, showsIndicators: false) {
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
    .foregroundStyle(isEnabled ? .black : .gray)
  }
}

// MARK: - Mini Meters

struct MiniMetersView: View {
  @EnvironmentObject var meters: MeterState

  var body: some View {
    VStack(spacing: 6) {
      MiniMeterRow(label: "L", peak: meters.playbackPeak.left, rms: meters.playbackRms.left)
      MiniMeterRow(label: "R", peak: meters.playbackPeak.right, rms: meters.playbackRms.right)
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
