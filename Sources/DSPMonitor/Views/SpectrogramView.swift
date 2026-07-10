// SpectrogramView - waterfall plot showing frequency history over time

import AppKit
import SwiftUI

private let bottomPadding: CGFloat = 20
private let leftPadding: CGFloat = 40

struct SpectrogramView: View {
  @Environment(SpectrogramEngine.self) var spectroscope

  var body: some View {
    if spectroscope.show3D {
      CSDWaterfallView()
    } else {
      ZStack {
        // Static grid and labels layer
        SpectrogramGridView(frequencies: spectroscope.frequencies, nBins: spectroscope.nBins)
          .equatable()

        // Dynamic waterfall layer
        SpectrogramContentView(leftPadding: leftPadding, bottomPadding: bottomPadding)
      }
    }
  }
}

struct SpectrogramContentView: View {
  @Environment(SpectrogramEngine.self) var spectroscope

  let leftPadding: CGFloat
  let bottomPadding: CGFloat

  @State private var bitmapContext: CGContext?
  @State private var currentX: CGFloat = 0.0
  @State private var bufferSize: CGSize = .zero
  @State private var publishedImage: CGImage?
  @State private var lastUpdateTime: Date = .distantPast

  var body: some View {
    GeometryReader { geometry in
      Canvas { context, size in
        if let image = publishedImage {
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
        let now = Date()
        let elapsed = lastUpdateTime == .distantPast ? 0.05 : now.timeIntervalSince(lastUpdateTime)
        if elapsed >= 0.05 {
          updateBuffer(with: newHistory, size: geometry.size, elapsed: elapsed)
          lastUpdateTime = now
        }
      }
      .onChange(of: geometry.size) { _, newSize in
        recreateBuffer(size: newSize, history: spectroscope.history)
      }
      .onAppear {
        recreateBuffer(size: geometry.size, history: spectroscope.history)
      }
    }
  }

  private func updateBuffer(with history: [SpectrogramFrame], size: CGSize, elapsed: TimeInterval) {
    guard let context = bitmapContext else { return }

    guard let lastFrame = history.last else {
      // History cleared, clear the buffer
      context.clear(CGRect(origin: .zero, size: size))
      publishedImage = context.makeImage()
      currentX = 0
      return
    }

    let drawWidth = size.width - leftPadding
    let drawHeight = size.height - bottomPadding

    let stripWidth = drawWidth * CGFloat(elapsed / 10.0)
    let clearWidth = max(1.0, ceil(stripWidth))

    // Clear stale portion before drawing new data
    context.clear(
      CGRect(x: leftPadding + currentX, y: 0, width: clearWidth, height: size.height))
    if currentX + clearWidth > drawWidth {
      let wrappedWidth = (currentX + clearWidth) - drawWidth
      context.clear(CGRect(x: leftPadding, y: 0, width: wrappedWidth, height: size.height))
    }

    // Draw new data at currentX
    drawFrame(
      lastFrame, in: context, at: leftPadding + currentX, width: stripWidth, drawHeight: drawHeight,
      nBins: spectroscope.nBins)

    // Advance cursor
    currentX += stripWidth
    if currentX >= drawWidth {
      currentX -= drawWidth  // Keep fraction for accurate wrap
    }

    // Publish the new image
    publishedImage = context.makeImage()
  }

  private func recreateBuffer(size: CGSize, history: [SpectrogramFrame]) {
    guard size.width > 0 && size.height > 0 else { return }

    let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
    let bitmapInfo = CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)

    guard
      let context = CGContext(
        data: nil, width: Int(size.width), height: Int(size.height), bitsPerComponent: 8,
        bytesPerRow: Int(size.width) * 4, space: colorSpace, bitmapInfo: bitmapInfo.rawValue)
    else { return }

    let drawWidth = size.width - leftPadding
    let drawHeight = size.height - bottomPadding

    // Redraw all history
    redrawAllHistory(
      in: context, history: history, size: size, drawWidth: drawWidth,
      drawHeight: drawHeight)

    self.bitmapContext = context
    self.publishedImage = context.makeImage()
    self.bufferSize = size
    self.currentX = 0  // Reset cursor on resize
  }

  private func redrawAllHistory(
    in context: CGContext, history: [SpectrogramFrame], size: CGSize,
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

      let cacheIndex = min(255, max(0, Int(normalized * 255.0)))
      let cgColor = spectrogramColorCache[cacheIndex]

      let y = bottomPadding + CGFloat(j) * barHeight
      let rect = CGRect(x: x, y: y, width: width, height: barHeight)

      context.setFillColor(cgColor)
      context.fill(rect)
    }
  }
}

private let spectrogramColorCache: [CGColor] = {
  var cache = [CGColor]()
  cache.reserveCapacity(256)
  for i in 0..<256 {
    let normalized = Float(i) / 255.0
    let alpha = normalized < 0.2 ? CGFloat(normalized / 0.2) : 1.0
    let v = Double(normalized)
    let r: CGFloat
    let g: CGFloat
    let b: CGFloat

    if v < 0.35 {
      r = 0.0; g = 1.0; b = 0.0
    } else if v < 0.55 {
      let t = (v - 0.35) / 0.2
      r = t; g = 1.0; b = 0.0
    } else if v < 0.75 {
      let t = (v - 0.55) / 0.2
      r = 1.0; g = 1.0 - t * 0.5; b = 0.0
    } else if v < 0.95 {
      let t = (v - 0.75) / 0.2
      r = 1.0; g = 0.5 - t * 0.5; b = 0.0
    } else {
      r = 1.0; g = 0.0; b = 0.0
    }

    cache.append(CGColor(srgbRed: r, green: g, blue: b, alpha: alpha))
  }
  return cache
}()

struct SpectrogramGridView: View, Equatable {
  let frequencies: [Float]?
  let nBins: UInt32

  nonisolated static func == (lhs: SpectrogramGridView, rhs: SpectrogramGridView) -> Bool {
    lhs.frequencies == rhs.frequencies && lhs.nBins == rhs.nBins
  }

  var body: some View {
    Canvas { context, size in
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
          at: CGPoint(x: leftPadding / 2, y: y),
          anchor: target == 20 ? .bottomLeading : .topLeading)
      }
    }
  }
}

// MARK: - 3D CSD Waterfall Landscape

struct CSDWaterfallView: View {
  @Environment(SpectrogramEngine.self) var spectroscope

  var body: some View {
    GeometryReader { geometry in
      Canvas { context, size in
        let history = spectroscope.history
        let count = history.count
        guard count > 1 else {
          context.draw(
            Text("Waiting for audio signal...").font(.headline).foregroundColor(.secondary),
            at: CGPoint(x: size.width / 2, y: size.height / 2),
            anchor: .center)
          return
        }

        let w = size.width
        let h = size.height

        let maxShiftX: CGFloat = w * 0.12
        let maxShiftY: CGFloat = -h * 0.18
        let maxScale: CGFloat = 0.85

        let baselineY = h * 0.88
        let drawWidth = w * 0.82
        let drawHeight = h * 0.58
        let leftPadding = w * 0.05

        func project(flatX: CGFloat, flatY: CGFloat, t: Double) -> CGPoint {
          let shiftX = (1.0 - t) * maxShiftX
          let shiftY = (1.0 - t) * maxShiftY
          let scale = maxScale + (1.0 - maxScale) * t
          let currentCenter = CGPoint(x: leftPadding + drawWidth / 2, y: baselineY)
          let px = currentCenter.x + (flatX - currentCenter.x) * scale + shiftX
          let py = currentCenter.y + (flatY - currentCenter.y) * scale + shiftY
          return CGPoint(x: px, y: py)
        }

        // Draw 3D floor grid lines (Time Grid)
        for fraction in [0.0, 0.2, 0.4, 0.6, 0.8, 1.0] {
          var path = Path()
          let xFlat = leftPadding + CGFloat(fraction) * drawWidth
          let ptStart = project(flatX: xFlat, flatY: baselineY, t: 0.0)
          path.move(to: ptStart)
          for i in 1..<count {
            let t = Double(i) / Double(count - 1)
            let pt = project(flatX: xFlat, flatY: baselineY, t: t)
            path.addLine(to: pt)
          }
          context.stroke(path, with: .color(Color.primary.opacity(0.12)), lineWidth: 0.5)
        }

        // Draw 3D floor grid lines (Frequency/Depth Grid)
        for t in [0.0, 0.25, 0.5, 0.75, 1.0] {
          var path = Path()
          let ptLeft = project(flatX: leftPadding, flatY: baselineY, t: t)
          let ptRight = project(flatX: leftPadding + drawWidth, flatY: baselineY, t: t)
          path.move(to: ptLeft)
          path.addLine(to: ptRight)
          context.stroke(path, with: .color(Color.primary.opacity(0.12)), lineWidth: 0.5)
        }

        // Resolve colors once outside the loop
        let startResolved = Color.blue.opacity(0.3).resolve(in: context.environment)
        let endResolved = Color.accentColor.resolve(in: context.environment)
        let r1 = startResolved.red
        let g1 = startResolved.green
        let b1 = startResolved.blue
        let a1: Float = 0.3
        let r2 = endResolved.red
        let g2 = endResolved.green
        let b2 = endResolved.blue
        let a2: Float = 1.0

        let controlBgResolved = Color(nsColor: .controlBackgroundColor).resolve(in: context.environment)
        let controlBgColor = Color(controlBgResolved).opacity(0.92)

        // Draw stacked curves from back to front
        for i in 0..<count {
          let frame = history[i]
          let t = Double(i) / Double(count - 1)

          let nBins = frame.data.count
          guard nBins > 2 else { continue }

          let startPt = project(flatX: leftPadding, flatY: baselineY, t: t)

          // 1. Build the filled shape path using a flat coordinates array
          let drawBins = min(Int(nBins), 100)
          var points: [CGPoint] = []
          points.reserveCapacity(drawBins + 3)
          points.append(startPt)

          for k in 0..<drawBins {
            let j = Int(round(Double(k) / Double(drawBins - 1) * Double(nBins - 1)))
            let binFrac = Double(j) / Double(nBins - 1)
            let xFlat = leftPadding + CGFloat(binFrac) * drawWidth
            let magnitude = frame.data[j]
            let normMag = normalizedDB(magnitude)
            let yFlat = baselineY - CGFloat(normMag) * drawHeight
            let projPt = project(flatX: xFlat, flatY: yFlat, t: t)
            points.append(projPt)
          }

          let endPt = project(flatX: leftPadding + drawWidth, flatY: baselineY, t: t)
          points.append(endPt)
          points.append(startPt)

          var path = Path()
          path.addLines(points)

          context.fill(path, with: .color(controlBgColor))

          // 2. Build the top edge wave path using a flat coordinates array
          var edgePoints: [CGPoint] = []
          edgePoints.reserveCapacity(drawBins)

          for k in 0..<drawBins {
            let j = Int(round(Double(k) / Double(drawBins - 1) * Double(nBins - 1)))
            let binFrac = Double(j) / Double(nBins - 1)
            let xFlat = leftPadding + CGFloat(binFrac) * drawWidth
            let yFlat = baselineY - CGFloat(normalizedDB(frame.data[j])) * drawHeight
            let projPt = project(flatX: xFlat, flatY: yFlat, t: t)
            edgePoints.append(projPt)
          }

          var edgePath = Path()
          edgePath.addLines(edgePoints)

          // Interpolate color components directly
          let tf = Float(t)
          let r = r1 + tf * (r2 - r1)
          let g = g1 + tf * (g2 - g1)
          let b = b1 + tf * (b2 - b1)
          let a = a1 + tf * (a2 - a1)
          let ageColor = Color(red: Double(r), green: Double(g), blue: Double(b), opacity: Double(a))

          context.stroke(edgePath, with: .color(ageColor), lineWidth: 1.5)
        }

        // Draw frequency labels at the front (t = 1.0)
        let targetFreqs: [Float] = [20, 100, 1000, 10000, 20000]
        let minLog = log10(20.0)
        let maxLog = log10(20000.0)
        for target in targetFreqs {
          let fraction = (log10(Double(target)) - minLog) / (maxLog - minLog)
          let xFlat = leftPadding + CGFloat(fraction) * drawWidth
          let pt = project(flatX: xFlat, flatY: baselineY + 8, t: 1.0)
          let label = formatFrequency(target)
          context.draw(
            Text(label).font(.system(size: 8, design: .monospaced)).foregroundColor(
              .secondary.opacity(0.7)),
            at: pt,
            anchor: .top)
        }
      }
    }
  }
}


