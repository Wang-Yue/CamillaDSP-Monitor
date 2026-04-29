// SpectrogramView - waterfall plot showing frequency history over time

import AppKit
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
  @State private var bitmapContext: CGContext?
  @State private var currentX: CGFloat = 0.0
  @State private var bufferSize: CGSize = .zero
  @State private var publishedImage: CGImage?

  var body: some View {
    GeometryReader { geometry in
      Canvas { context, size in
        if let image = publishedImage {
          let leftPadding: CGFloat = 40
          let drawWidth = size.width - leftPadding

          // Part 1: Oldest data (cursorX to right edge -> drawn on the left)
          let leftPartWidth = drawWidth - currentX
          if leftPartWidth > 0 {
            var subContext = context
            let clipRect = CGRect(x: leftPadding, y: 0, width: leftPartWidth, height: size.height)
            subContext.clip(to: Path(clipRect))

            let imageRect = CGRect(x: -currentX, y: 0, width: size.width, height: size.height)
            subContext.draw(Image(image, scale: 1.0, label: Text("")), in: imageRect)
          }

          // Part 2: Newest data (leftPadding to cursorX -> drawn on the right)
          let rightPartWidth = currentX
          if rightPartWidth > 0 {
            var subContext = context
            let clipRect = CGRect(
              x: leftPadding + leftPartWidth, y: 0, width: rightPartWidth, height: size.height)
            subContext.clip(to: Path(clipRect))

            let imageRect = CGRect(
              x: drawWidth - currentX, y: 0, width: size.width, height: size.height)
            subContext.draw(Image(image, scale: 1.0, label: Text("")), in: imageRect)
          }
        }
      }
      .onChange(of: spectroscope.history) { _, newHistory in
        updateBuffer(with: newHistory, size: geometry.size)
      }
      .onChange(of: geometry.size) { _, newSize in
        recreateBuffer(size: newSize, history: spectroscope.history)
      }
      .onAppear {
        recreateBuffer(size: geometry.size, history: spectroscope.history)
      }
    }
  }

  private func updateBuffer(with history: [SpectrogramFrame], size: CGSize) {
    guard let context = bitmapContext else { return }

    guard let lastFrame = history.last else {
      // History cleared, clear the buffer
      context.clear(CGRect(origin: .zero, size: size))
      publishedImage = context.makeImage()
      currentX = 0
      return
    }

    let leftPadding: CGFloat = 40
    let drawWidth = size.width - leftPadding
    let drawHeight = size.height - 20  // bottomPadding = 20

    let count = history.count
    let timeDiff: TimeInterval
    if count > 1 {
      timeDiff = lastFrame.timestamp.timeIntervalSince(history[count - 2].timestamp)
    } else {
      timeDiff = 0.1  // Fallback
    }

    let stripWidth = drawWidth * CGFloat(timeDiff / 10.0)
    let gapWidth: CGFloat = 10.0
    let totalClearWidth = stripWidth + gapWidth

    // Clear stale portion and a gap ahead before drawing new data
    context.clear(
      CGRect(x: leftPadding + currentX, y: 0, width: totalClearWidth, height: size.height))
    if currentX + totalClearWidth > drawWidth {
      let wrappedWidth = (currentX + totalClearWidth) - drawWidth
      context.clear(CGRect(x: leftPadding, y: 0, width: wrappedWidth, height: size.height))
    }

    // Draw new data at currentX
    drawFrame(
      lastFrame, in: context, at: leftPadding + currentX, width: stripWidth, drawHeight: drawHeight,
      nBins: spectroscope.nBins)

    // Advance cursor
    currentX += stripWidth
    if currentX >= drawWidth {
      currentX = 0  // Wrap around
    }

    // Publish the new image
    publishedImage = context.makeImage()
  }

  private func recreateBuffer(size: CGSize, history: [SpectrogramFrame]) {
    guard size.width > 0 && size.height > 0 else { return }

    let colorSpace = CGColorSpaceCreateDeviceRGB()
    let bitmapInfo = CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue)

    guard
      let context = CGContext(
        data: nil, width: Int(size.width), height: Int(size.height), bitsPerComponent: 8,
        bytesPerRow: Int(size.width) * 4, space: colorSpace, bitmapInfo: bitmapInfo.rawValue)
    else { return }

    let leftPadding: CGFloat = 40
    let bottomPadding: CGFloat = 20
    let drawWidth = size.width - leftPadding
    let drawHeight = size.height - bottomPadding

    // Redraw all history
    redrawAllHistory(
      in: context, history: history, size: size, leftPadding: leftPadding, drawWidth: drawWidth,
      drawHeight: drawHeight)

    self.bitmapContext = context
    self.publishedImage = context.makeImage()
    self.bufferSize = size
    self.currentX = 0  // Reset cursor on resize
  }

  private func redrawAllHistory(
    in context: CGContext, history: [SpectrogramFrame], size: CGSize, leftPadding: CGFloat,
    drawWidth: CGFloat, drawHeight: CGFloat
  ) {
    let nBins = spectroscope.nBins
    let now = Date()

    context.clear(CGRect(origin: .zero, size: size))

    let count = history.count
    for i in 0..<count {
      let frame = history[i]
      let timeAgo = now.timeIntervalSince(frame.timestamp)
      guard timeAgo <= 10.0 else { continue }

      let x = leftPadding + drawWidth * (1.0 - timeAgo / 10.0)

      let nextX: CGFloat
      if i < count - 1 {
        let nextTimeAgo = now.timeIntervalSince(history[i + 1].timestamp)
        nextX = leftPadding + drawWidth * (1.0 - nextTimeAgo / 10.0)
      } else {
        nextX = size.width
      }

      let stripWidth = max(1.0, nextX - x)

      drawFrame(frame, in: context, at: x, width: stripWidth, drawHeight: drawHeight, nBins: nBins)
    }
  }

  private func drawFrame(
    _ frame: SpectrogramFrame, in context: CGContext, at x: CGFloat, width: CGFloat,
    drawHeight: CGFloat, nBins: UInt32
  ) {
    let barHeight = drawHeight / CGFloat(nBins)

    for j in 0..<min(Int(nBins), frame.data.count) {
      let magnitude = frame.data[j]
      let normalized = Float(normalizedDB(magnitude))

      // Skip very low signals as optimization
      guard normalized > 0.05 else { continue }

      let baseColor = appThemeColor(normalized)
      let color = normalized < 0.2 ? baseColor.opacity(Double(normalized / 0.2)) : baseColor

      let bottomPadding: CGFloat = 20
      let y = bottomPadding + CGFloat(j) * barHeight
      let rect = CGRect(x: x, y: y, width: width, height: barHeight)

      let nsColor = NSColor(color)
      context.setFillColor(nsColor.cgColor)
      context.fill(rect)
    }
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
      // Draw frequency labels (Fixed positions independent of bins)
      let targetFreqs: [Float] = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]
      let minLog = log10(20.0)
      let maxLog = log10(20000.0)
      for target in targetFreqs {
        let fraction = (log10(Double(target)) - minLog) / (maxLog - minLog)
        let y = drawHeight * (1.0 - CGFloat(fraction))

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
          anchor: target == 20 ? .bottomLeading : .topLeading)
      }
    }
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
