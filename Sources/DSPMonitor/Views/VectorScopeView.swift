// VectorScopeView - Goniometer visualization

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
        // Draw dynamic decaying phosphor particles
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
          let alpha = 0.03 + 0.82 * t
          let baseColor = Color.interpolate(from: .indigo, to: .cyan, fraction: t)

          // Glowing halo overlay for head of vector stream
          if t > 0.9 {
            let glowSize = size * 2.0
            let glowRect = CGRect(
              x: px - glowSize / 2, y: py - glowSize / 2, width: glowSize, height: glowSize)
            context.fill(Path(ellipseIn: glowRect), with: .color(baseColor.opacity(alpha * 0.25)))
          }

          let rect = CGRect(x: px - size / 2, y: py - size / 2, width: size, height: size)
          context.fill(Path(ellipseIn: rect), with: .color(baseColor.opacity(alpha)))
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
