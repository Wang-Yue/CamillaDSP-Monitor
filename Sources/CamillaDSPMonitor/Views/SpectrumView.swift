// SpectrumView - spectrum analyzer display with gradient bars

import Observation
import SwiftUI

private let spectrumTopPadding: CGFloat = 10
private let spectrumLeftPadding: CGFloat = 20

// MARK: - SpectrumView

struct SpectrumView: View {
  let bands: [Float]?  // dB values for each band
  let frequencies: [Float]?  // Add this

  var body: some View {
    ZStack {
      // Static grid layer — only redraws on size change, not on band value updates.
      // This avoids expensive CoreText/ICU text resolution every 100ms frame.
      SpectrumGridView(frequencies: frequencies).equatable()

      // Dynamic bars layer — redraws at 10 Hz with band data
      if let bands = bands {
        Canvas { context, size in
          context.translateBy(x: 0, y: spectrumTopPadding)
          drawSpectrumBars(
            context: &context, bands: bands,
            maxHeight: size.height - spectrumLeftPadding - spectrumTopPadding,
            totalWidth: size.width - spectrumLeftPadding,
            xOffset: spectrumLeftPadding)
        }
      }
    }
  }
}

/// Static grid overlay for SpectrumView. Separated so SwiftUI only redraws it
/// when the view size changes, not on every band data update (10 Hz).
private struct SpectrumGridView: View, Equatable {
  let frequencies: [Float]?  // Add this

  nonisolated static func == (lhs: SpectrumGridView, rhs: SpectrumGridView) -> Bool {
    lhs.frequencies == rhs.frequencies
  }

  private static let dbMarks = [0, -12, -24, -36, -48, -60]

  var body: some View {
    Canvas { context, size in
      let maxHeight = size.height - spectrumLeftPadding - spectrumTopPadding
      let barSpacing: CGFloat = 2
      let bandCount = frequencies?.count ?? 30  // Fallback to 30 if nil
      let totalSpacing = barSpacing * CGFloat(bandCount - 1)
      let barWidth = max(4, (size.width - spectrumLeftPadding - totalSpacing) / CGFloat(bandCount))

      // dB grid lines and labels
      for dbMark in Self.dbMarks {
        let y = spectrumTopPadding + maxHeight * (1.0 - (Double(dbMark) + 60) / 60)

        var line = Path()
        line.move(to: CGPoint(x: spectrumLeftPadding, y: y))
        line.addLine(to: CGPoint(x: size.width, y: y))
        context.stroke(line, with: .color(Color.primary.opacity(0.05)), lineWidth: 0.5)

        context.draw(
          Text("\(dbMark)").font(.system(size: 8, design: .monospaced)).foregroundColor(
            .secondary.opacity(0.5)), at: CGPoint(x: 10, y: y))
      }

      // Frequency labels
      if let frequencies = frequencies {
        let count = frequencies.count
        for i in 0..<count {
          let f = frequencies[i]
          let label = formatFrequency(f)
          let x = CGFloat(i) * (barWidth + barSpacing) + spectrumLeftPadding
          context.draw(
            Text(label).font(.system(size: 7)).foregroundColor(
              .secondary.opacity(0.7)),
            at: CGPoint(x: x + barWidth / 2, y: spectrumTopPadding + maxHeight + 10))
        }
      }
    }
  }
}

private func formatFrequency(_ f: Float) -> String {
  if f >= 1000 {
    let k = f / 1000
    // Use at most 2 digits for fraction
    let s = String(format: "%.2f", k)
    let parts = s.split(separator: ".")
    let intPart = parts[0]
    let fracPart =
      parts.count > 1 ? parts[1].trimmingCharacters(in: CharacterSet(charactersIn: "0")) : ""

    if fracPart.isEmpty {
      return "\(intPart)k"
    } else {
      return "\(intPart)k\(fracPart)"
    }
  } else {
    // For < 1000, drop anything after the dot (round to nearest integer)
    return "\(Int(f.rounded()))"
  }
}
