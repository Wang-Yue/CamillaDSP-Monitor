import SwiftUI

struct VectorScopeView: View {
  @Environment(VectorScopeEngine.self) var vectorscope

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("Vector Scope").font(.headline)

      VectorScopeContentView(showGrid: true)
    }
    .padding()
    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
    .onAppear { vectorscope.visibilityCount += 1 }
    .onDisappear { vectorscope.visibilityCount -= 1 }
  }
}

struct VectorScopeContentView: View {
  @Environment(VectorScopeEngine.self) var vectorscope
  let showGrid: Bool

  var body: some View {
    Canvas { context, size in
      let drawWidth = size.width
      let drawHeight = size.height
      let center = CGPoint(x: drawWidth / 2, y: drawHeight / 2)

      let scaleX = drawWidth / 2
      let scaleY = drawHeight / 2

      if showGrid {
        // Draw background grid (axes)
        var gridPath = Path()
        gridPath.move(to: CGPoint(x: 0, y: center.y))
        gridPath.addLine(to: CGPoint(x: drawWidth, y: center.y))
        gridPath.move(to: CGPoint(x: center.x, y: 0))
        gridPath.addLine(to: CGPoint(x: center.x, y: drawHeight))
        context.stroke(gridPath, with: .color(Color.primary.opacity(0.1)), lineWidth: 1)

        // Draw diagonal lines (corner to corner)
        var diagPath = Path()
        diagPath.move(to: CGPoint(x: 0, y: 0))
        diagPath.addLine(to: CGPoint(x: drawWidth, y: drawHeight))
        diagPath.move(to: CGPoint(x: drawWidth, y: 0))
        diagPath.addLine(to: CGPoint(x: 0, y: drawHeight))
        context.stroke(diagPath, with: .color(Color.primary.opacity(0.05)), lineWidth: 0.5)
      }

      // Draw samples
      let left = vectorscope.leftSamples
      let right = vectorscope.rightSamples
      let count = min(left.count, right.count)

      guard count > 1 else { return }

      // Auto Scale logic to boost very low signals to full scale bounds
      var autoScaleFactor: CGFloat = 1.0
      if vectorscope.autoScale && count > 0 {
        var maxVal: Float = 0.0
        for i in 0..<count {
          let l = left[i]
          let r = right[i]
          let x = (l - r) / 1.414
          let y = (l + r) / 1.414
          let val = max(abs(x), abs(y))
          if val > maxVal { maxVal = val }
        }
        if maxVal > 1e-4 {
          let targetScale = 0.90 / CGFloat(maxVal)  // Leave 10% margin
          autoScaleFactor = min(targetScale, 32.0)  // Limit boost gain to 32x to avoid noise floor explosions
        }
      }

      if vectorscope.showParticles {
        let numBins = 32
        var binPaths = Array(repeating: Path(), count: numBins)
        var glowPaths = Array(repeating: Path(), count: numBins)

        // Resolve colors once outside the loop using SwiftUI's environment
        let startColor = Color.indigo.resolve(in: context.environment)
        let endColor = Color.cyan.resolve(in: context.environment)
        let r1 = startColor.red
        let g1 = startColor.green
        let b1 = startColor.blue
        let r2 = endColor.red
        let g2 = endColor.green
        let b2 = endColor.blue

        for i in 0..<count {
          let l = left[i]
          let r = right[i]

          // Mid/Side rotation
          let x = (l - r) / 1.414
          let y = (l + r) / 1.414

          // Map to view coordinates with auto-scaling
          let px = center.x + CGFloat(x) * scaleX * autoScaleFactor
          let py = center.y - CGFloat(y) * scaleY * autoScaleFactor

          let t = Double(i) / Double(count - 1)
          let size = 1.0 + 3.5 * t
          let binIndex = min(Int(t * Double(numBins)), numBins - 1)

          let rect = CGRect(x: px - size / 2, y: py - size / 2, width: size, height: size)
          binPaths[binIndex].addEllipse(in: rect)

          // Glowing halo overlay for head of vector stream
          if t > 0.9 {
            let glowSize = size * 2.0
            let glowRect = CGRect(
              x: px - glowSize / 2, y: py - glowSize / 2, width: glowSize, height: glowSize)
            glowPaths[binIndex].addEllipse(in: glowRect)
          }
        }

        for binIndex in 0..<numBins {
          let t = Float(binIndex) / Float(numBins - 1)
          let alpha = Float(0.03 + 0.82 * Double(t))

          let r = r1 + t * (r2 - r1)
          let g = g1 + t * (g2 - g1)
          let b = b1 + t * (b2 - b1)
          let color = Color(
            red: Double(r), green: Double(g), blue: Double(b), opacity: Double(alpha))

          if !glowPaths[binIndex].isEmpty {
            context.fill(glowPaths[binIndex], with: .color(color.opacity(0.25)))
          }
          if !binPaths[binIndex].isEmpty {
            context.fill(binPaths[binIndex], with: .color(color))
          }
        }
      } else {
        // Draw continuous line path
        var path = Path()
        var first = true

        for i in 0..<count {
          let l = left[i]
          let r = right[i]

          // Mid/Side rotation
          let x = (l - r) / 1.414
          let y = (l + r) / 1.414

          // Map to view coordinates with auto-scaling
          let px = center.x + CGFloat(x) * scaleX * autoScaleFactor
          let py = center.y - CGFloat(y) * scaleY * autoScaleFactor

          let point = CGPoint(x: px, y: py)

          if first {
            path.move(to: point)
            first = false
          } else {
            path.addLine(to: point)
          }
        }

        let lineWidth = max(1.0, min(drawWidth, drawHeight) / 150.0)
        context.stroke(path, with: .color(Color.accentColor.opacity(0.7)), lineWidth: lineWidth)
      }
    }
  }
}
