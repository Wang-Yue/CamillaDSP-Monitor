// SpectrumView - 30-band spectrum analyzer display with gradient bars

import SwiftUI

// MARK: - SpectrumView

struct SpectrumView: View {
  let bands: [Double]  // dB values for each band

  var body: some View {
    ZStack {
      // Static grid layer — only redraws on size change, not on band value updates.
      // This avoids expensive CoreText/ICU text resolution every 100ms frame.
      SpectrumGridView()

      // Dynamic bars layer — redraws at 10 Hz with band data
      Canvas { context, size in
        drawSpectrumBars(
          context: &context, bands: bands,
          maxHeight: size.height - 20, totalWidth: size.width - 20,
          xOffset: 20)
      }
    }
  }
}

/// Static grid overlay for SpectrumView. Separated so SwiftUI only redraws it
/// when the view size changes, not on every band data update (10 Hz).
private struct SpectrumGridView: View {
  private static let labels: [String] = [
    "25", "31.5", "40", "50", "63", "80", "100", "125", "160", "200", "250",
    "315", "400", "500", "630", "800", "1k", "1k25", "1k6", "2k", "2k5",
    "3k15", "4k", "5k", "6k3", "8k", "10k", "12k5", "16k", "20k",
  ]

  private static let dbMarks = [0, -12, -24, -36, -48, -60]

  var body: some View {
    Canvas { context, size in
      let maxHeight = size.height - 20
      let barSpacing: CGFloat = 2
      let bandCount = 30
      let totalSpacing = barSpacing * CGFloat(bandCount - 1)
      let barWidth = max(4, (size.width - 20 - totalSpacing) / CGFloat(bandCount))

      // dB grid lines and labels
      for dbMark in Self.dbMarks {
        let y = maxHeight * (1.0 - (Double(dbMark) + 60) / 60)

        var line = Path()
        line.move(to: CGPoint(x: 20, y: y))
        line.addLine(to: CGPoint(x: size.width, y: y))
        context.stroke(line, with: .color(Color.primary.opacity(0.05)), lineWidth: 0.5)

        context.draw(
          Text("\(dbMark)").font(.system(size: 8, design: .monospaced)).foregroundColor(
            .secondary.opacity(0.5)), at: CGPoint(x: 10, y: y))
      }

      // Frequency labels
      for i in 0..<bandCount {
        if !Self.labels[i].isEmpty {
          let x = CGFloat(i) * (barWidth + barSpacing) + 20
          context.draw(
            Text(Self.labels[i]).font(.system(size: 7)).foregroundColor(
              .secondary.opacity(0.7)),
            at: CGPoint(x: x + barWidth / 2, y: maxHeight + 10))
        }
      }
    }
  }
}
