// SpectrumView - 30-band spectrum analyzer display with gradient bars

import SwiftUI

struct SpectrumView: View {
  let bands: [Double]  // dB values for each band

  static let labels: [String] = [
    "25", "", "40", "", "63", "", "100", "", "160", "",
    "250", "", "400", "", "630", "", "1k", "", "1.6k", "",
    "2.5k", "", "4k", "", "6.3k", "", "10k", "", "16k", "20k",
  ]

  private let barGradient = Gradient(stops: [
    .init(color: .green, location: 0.0),
    .init(color: .green, location: 0.35),
    .init(color: .yellow, location: 0.55),
    .init(color: .orange, location: 0.75),
    .init(color: .red, location: 0.95),
    .init(color: .red, location: 1.0),
  ])

  var body: some View {
    Canvas { context, size in
      let maxHeight = size.height - 20
      let barSpacing: CGFloat = 2
      let totalSpacing = barSpacing * CGFloat(max(0, bands.count - 1))
      let barWidth = max(4, (size.width - 20 - totalSpacing) / CGFloat(max(1, bands.count)))

      // Draw Grid
      for dbMark in [0, -12, -24, -36, -48, -60] {
        let y = maxHeight * (1.0 - (Double(dbMark) + 60) / 60)

        // Grid Line
        var line = Path()
        line.move(to: CGPoint(x: 20, y: y))
        line.addLine(to: CGPoint(x: size.width, y: y))
        context.stroke(line, with: .color(Color.primary.opacity(0.05)), lineWidth: 0.5)

        // Grid Label
        context.draw(
          Text("\(dbMark)").font(.system(size: 8, design: .monospaced)).foregroundColor(
            .secondary.opacity(0.5)), at: CGPoint(x: 10, y: y))
      }

      // Draw Bars and Labels
      for i in 0..<min(bands.count, 30) {
        let x = CGFloat(i) * (barWidth + barSpacing) + 20  // Offset for grid labels
        let normalized = normalizedDB(bands[i])
        let barHeight = max(2, Double(maxHeight) * normalized)

        // Bar
        let barRect = CGRect(
          x: x, y: CGFloat(Double(maxHeight) - barHeight), width: barWidth,
          height: CGFloat(barHeight))
        context.fill(
          Path(roundedRect: barRect, cornerRadius: 2),
          with: .linearGradient(
            barGradient, startPoint: CGPoint(x: x, y: maxHeight), endPoint: CGPoint(x: x, y: 0)))

        // Label
        if !Self.labels[i].isEmpty {
          context.draw(
            Text(Self.labels[i]).font(.system(size: 7)).foregroundColor(.secondary.opacity(0.7)),
            at: CGPoint(x: x + barWidth / 2, y: maxHeight + 10))
        }
      }
    }
  }
}
