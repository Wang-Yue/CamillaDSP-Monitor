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

      var path = Path()
      var first = true

      for i in 0..<count {
        let l = left[i]
        let r = right[i]

        // Mid/Side rotation
        let x = (l - r) / 1.414
        let y = (l + r) / 1.414

        // Map to view coordinates with independent scales
        let px = center.x + CGFloat(x) * scaleX
        let py = center.y - CGFloat(y) * scaleY

        let point = CGPoint(x: px, y: py)

        if first {
          path.move(to: point)
          first = false
        } else {
          path.addLine(to: point)
        }
      }

      context.stroke(path, with: .color(Color.accentColor.opacity(0.7)), lineWidth: 1)
    }
  }
}
