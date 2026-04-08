// SpectrumView - 30-band spectrum analyzer display with gradient bars

import SwiftUI

struct SpectrumView: View {
  let bands: [Double]  // dB values for each band

  static let labels: [String] = [
    "25", "", "40", "", "63", "", "100", "", "160", "",
    "250", "", "400", "", "630", "", "1k", "", "1.6k", "",
    "2.5k", "", "4k", "", "6.3k", "", "10k", "", "16k", "20k",
  ]

  var body: some View {
    GeometryReader { geo in
      let barWidth = max(4, (geo.size.width - CGFloat(bands.count - 1) * 2) / CGFloat(bands.count))
      let maxHeight = geo.size.height - 20

      ZStack(alignment: .bottom) {
        // Background grid
        ForEach([0, -12, -24, -36, -48, -60], id: \.self) { db in
          let y = maxHeight * (1.0 - (Double(db) + 60) / 60)
          HStack(spacing: 4) {
            Text("\(db)")
              .font(.system(size: 8, design: .monospaced))
              .foregroundStyle(.quaternary)
            Rectangle()
              .fill(Color.primary.opacity(0.05))
              .frame(height: 0.5)
          }
          .position(x: geo.size.width / 2, y: y)
        }

        // Bars
        HStack(alignment: .bottom, spacing: 2) {
          ForEach(0..<min(bands.count, 30), id: \.self) { i in
            let normalized = normalizedDB(bands[i])
            let height = max(2, maxHeight * normalized)

            VStack(spacing: 2) {
              SpectrumBar(height: height, width: barWidth, maxHeight: maxHeight)

              Text(Self.labels[i])
                .font(.system(size: 7))
                .foregroundStyle(.tertiary)
                .frame(height: 14)
            }
          }
        }
      }
    }
    .drawingGroup()  // Flatten into single Metal texture — avoids per-bar CoreAnimation layers
  }
}

// MARK: - Gradient Bar

struct SpectrumBar: View {
  let height: Double
  let width: Double
  let maxHeight: Double

  var body: some View {
    // The gradient always spans the full height range (0dB at top, -60dB at bottom).
    // We clip to the actual bar height so the color at the top of the bar matches
    // the level — short bars are green, tall bars show red at the top.
    RoundedRectangle(cornerRadius: 2)
      .fill(
        LinearGradient(
          stops: [
            .init(color: .green, location: 0.0),
            .init(color: .green, location: 0.35),
            .init(color: .yellow, location: 0.55),
            .init(color: .orange, location: 0.75),
            .init(color: .red, location: 0.95),
            .init(color: .red, location: 1.0),
          ],
          startPoint: .bottom,
          endPoint: .top
        )
      )
      .frame(width: width, height: height)
  }
}
