// SpectrogramView - waterfall plot showing frequency history over time

import SwiftUI

struct SpectrogramView: View {
  @Environment(SpectrogramEngine.self) var spectroscope

  var body: some View {
    ZStack {
      // Static grid and labels layer
      SpectrogramGridView(frequencies: spectroscope.frequencies, nBins: spectroscope.nBins)
        .equatable()

      // Dynamic waterfall layer
      SpectrogramContentView()
    }
  }
}

struct SpectrogramContentView: View {
  @Environment(SpectrogramEngine.self) var spectroscope

  var body: some View {
    Canvas { context, size in
      let history = spectroscope.history
      let count = history.count
      guard count > 0 else { return }

      let leftPadding: CGFloat = 40
      let bottomPadding: CGFloat = 20
      let drawWidth = size.width - leftPadding
      let drawHeight = size.height - bottomPadding

      let nBins = spectroscope.nBins
      let barHeight = drawHeight / CGFloat(nBins)
      let now = Date()

      for i in 0..<count {
        let frame = history[i]
        let timeAgo = now.timeIntervalSince(frame.timestamp)

        // Ignore frames outside the 10-second window
        guard timeAgo <= 10.0 else { continue }

        let x = leftPadding + drawWidth * (1.0 - timeAgo / 10.0)

        // Calculate width to fill the gap to the next frame (or edge)
        let nextX: CGFloat
        if i < count - 1 {
          let nextTimeAgo = now.timeIntervalSince(history[i + 1].timestamp)
          nextX = leftPadding + drawWidth * (1.0 - nextTimeAgo / 10.0)
        } else {
          nextX = size.width  // Newest frame fills to the right edge
        }

        let stripWidth = max(1.0, nextX - x)

        for j in 0..<min(Int(nBins), frame.data.count) {
          let magnitude = frame.data[j]
          let normalized = Float(normalizedDB(magnitude))
          let color = colorForMagnitude(normalized)

          // Low frequencies at the bottom of the draw area
          let y = drawHeight - CGFloat(j + 1) * barHeight
          let rect = CGRect(x: x, y: y, width: stripWidth, height: barHeight)

          context.fill(Path(rect), with: .color(color))
        }
      }
    }
  }

  private func colorForMagnitude(_ value: Float) -> Color {
    let baseColor = appThemeColor(value)

    // Apply opacity for low values to reveal the background
    if value < 0.2 {
      return baseColor.opacity(Double(value / 0.2))
    }
    return baseColor
  }

}

struct SpectrogramGridView: View, Equatable {
  let frequencies: [Float]?
  let nBins: UInt32

  nonisolated static func == (lhs: SpectrogramGridView, rhs: SpectrogramGridView) -> Bool {
    lhs.frequencies == rhs.frequencies && lhs.nBins == rhs.nBins
  }

  var body: some View {
    Canvas { context, size in
      let leftPadding: CGFloat = 40
      let bottomPadding: CGFloat = 20
      let drawWidth = size.width - leftPadding
      let drawHeight = size.height - bottomPadding

      // Draw time labels (0s to -10s)
      let timeMarks = [0, -2, -4, -6, -8, -10]
      for mark in timeMarks {
        let x = leftPadding + drawWidth * (1.0 - CGFloat(-mark) / 10.0)

        // Grid line
        var line = Path()
        line.move(to: CGPoint(x: x, y: 0))
        line.addLine(to: CGPoint(x: x, y: drawHeight))
        context.stroke(line, with: .color(Color.primary.opacity(0.05)), lineWidth: 0.5)

        // Label
        context.draw(
          Text("\(mark)s").font(.system(size: 8, design: .monospaced)).foregroundColor(.secondary),
          at: CGPoint(x: x, y: drawHeight + 10),
          anchor: mark == 0 ? .trailing : .center)
      }

      // Draw frequency labels
      if let freqs = frequencies {
        let targetFreqs: [Float] = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]
        for target in targetFreqs {
          if let index = findClosestIndex(target: target, in: freqs) {
            let y = drawHeight - CGFloat(index + 1) * (drawHeight / CGFloat(nBins))

            // Grid line
            var line = Path()
            line.move(to: CGPoint(x: leftPadding, y: y))
            line.addLine(to: CGPoint(x: size.width, y: y))
            context.stroke(line, with: .color(Color.primary.opacity(0.05)), lineWidth: 0.5)

            // Label
            let label = formatFrequency(target)
            context.draw(
              Text(label).font(.system(size: 8, design: .monospaced)).foregroundColor(.secondary),
              at: CGPoint(x: 20, y: y),
              anchor: .topLeading)
          }
        }
      }
    }
  }

  private func findClosestIndex(target: Float, in array: [Float]) -> Int? {
    guard !array.isEmpty else { return nil }
    var closestIndex = 0
    var minDiff = abs(array[0] - target)
    for i in 1..<array.count {
      let diff = abs(array[i] - target)
      if diff < minDiff {
        minDiff = diff
        closestIndex = i
      }
    }
    return closestIndex
  }

  private func formatFrequency(_ f: Float) -> String {
    if f >= 1000 {
      let k = f / 1000
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
      return "\(Int(f.rounded()))"
    }
  }
}
