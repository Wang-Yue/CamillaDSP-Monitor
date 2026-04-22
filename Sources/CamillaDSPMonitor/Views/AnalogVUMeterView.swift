import SwiftUI

// MARK: - VU Calibration Parameters
struct VUParams {
    var radiusScale: Double = 1.20
    var pivotY: Double = 1.55
    var needleExtension: Double = 45.0
    var ambientGlow: Double = 0.5
    var hotSpotAlpha: Double = 0.5
    var lightWash: Double = 0.2
}

// MARK: - Hyper-Realistic VU Meter

struct AnalogVUMeter: View {
  let level: Double // dBFS (-60...0)
  let label: String
  var params: VUParams = VUParams()
  var height: CGFloat = 160

  private let bulbHotSpotColor = Color(red: 1.0, green: 0.98, blue: 0.88)
  private let bulbAmberColor = Color(red: 1.0, green: 0.82, blue: 0.40)
  private let refLevel = -18.0 

  var body: some View {
    let scale = height / 160.0
    VStack(spacing: 8 * scale) {
      Canvas { context, size in
        drawVURenderer(context: &context, size: size, level: level, scale: scale)
      }
      .frame(height: height)
      .overlay(RoundedRectangle(cornerRadius: 6 * scale).stroke(Color.primary.opacity(0.2), lineWidth: 1.2 * scale))
      .clipShape(RoundedRectangle(cornerRadius: 6 * scale))
      
      Text(label)
        .font(.system(size: 11 * scale, weight: .black))
        .foregroundStyle(.secondary.opacity(0.8))
    }
    .padding(6 * scale)
  }

  private func drawVURenderer(context: inout GraphicsContext, size: CGSize, level: Double, scale: CGFloat) {
    let w = size.width
    let h = size.height
    
    let center = CGPoint(x: w / 2, y: h * params.pivotY)
    let radius = h * params.radiusScale 
    
    let startAngle = 235.0
    let endAngle = 305.0
    let totalSpan = endAngle - startAngle
    
    func angleForVU(_ vu: Double) -> Double {
        let ratio = pow(10.0, vu / 20.0)
        let minR = 0.1 
        let maxR = 1.412 
        let norm = (ratio - minR) / (maxR - minR)
        return startAngle + norm * totalSpan
    }

    // 1. BOTTOM AMBER GLOW
    let amberGlow = GraphicsContext.Shading.radialGradient(
        Gradient(stops: [
            .init(color: bulbAmberColor.opacity(params.ambientGlow), location: 0.0), 
            .init(color: bulbAmberColor.opacity(0.0), location: 0.8)
        ]),
        center: CGPoint(x: w/2, y: h + 10 * scale), startRadius: 0, endRadius: h * 1.6
    )
    context.fill(Path(CGRect(origin: .zero, size: size)), with: amberGlow)
    
    // 2. HOT SPOT
    let hotSpot = GraphicsContext.Shading.radialGradient(
        Gradient(stops: [
            .init(color: bulbHotSpotColor.opacity(params.hotSpotAlpha), location: 0.0),
            .init(color: bulbHotSpotColor.opacity(0.0), location: 1.0)
        ]),
        center: CGPoint(x: w/2, y: h + 5 * scale),
        startRadius: 0,
        endRadius: h * 0.4
    )
    context.fill(Path(CGRect(origin: .zero, size: size)), with: hotSpot)
    
    // 3. Main Arc
    context.stroke(Path { p in p.addArc(center: center, radius: radius, startAngle: .degrees(startAngle), endAngle: .degrees(endAngle), clockwise: false) }, with: .color(.primary.opacity(0.6)), lineWidth: 1.8 * scale)

    // 4. Rockwell Typography
    let vintageFont = Font.custom("Rockwell", size: 10 * scale)

    // VU Markings (ABOVE)
    let vuMarks: [(v: Double, l: String?)] = [
      (-20, "20"), (-10, "10"), (-7, "7"), (-5, "5"), (-3, "3"), (-2, "2"), (-1, "1"), (0, "0"), (1, "1"), (2, "2"), (3, "3")
    ]
    
    for m in vuMarks {
      let angDeg = angleForVU(m.v)
      let angRad = angDeg * .pi / 180
      let color = m.v >= 0 ? Color.red : Color.primary
      
      let cosA = cos(angRad)
      let sinA = sin(angRad)
      let s = CGPoint(x: center.x + cosA * radius, y: center.y + sinA * radius)
      let eR = radius + 7 * scale
      let e = CGPoint(x: center.x + cosA * eR, y: center.y + sinA * eR)
      
      context.stroke(Path { p in p.move(to: s); p.addLine(to: e) }, with: .color(color.opacity(0.7)), lineWidth: 1.8 * scale)
      
      if let text = m.l {
        let lR = radius + 18 * scale
        let lp = CGPoint(x: center.x + cosA * lR, y: center.y + sinA * lR)
        context.translateBy(x: lp.x, y: lp.y)
        context.rotate(by: .radians(angRad + .pi/2))
        context.draw(Text(text).font(vintageFont).foregroundColor(color.opacity(0.6)), at: .zero, anchor: .center)
        context.rotate(by: .radians(-(angRad + .pi/2)))
        context.translateBy(x: -lp.x, y: -lp.y)
      }
    }

    // Percentage Markings (BELOW)
    for p in [0, 20, 40, 60, 80, 100] {
      let ratio = Double(p) / 100.0
      let norm = (ratio - 0.1) / (1.412 - 0.1)
      let angRad = (startAngle + norm * totalSpan) * .pi / 180
      
      let cosA = cos(angRad)
      let sinA = sin(angRad)
      let s = CGPoint(x: center.x + cosA * radius, y: center.y + sinA * radius)
      let eR = radius - 7 * scale
      let e = CGPoint(x: center.x + cosA * eR, y: center.y + sinA * eR)
      
      context.stroke(Path { p in p.move(to: s); p.addLine(to: e) }, with: .color(.primary.opacity(0.4)), lineWidth: 1.0 * scale)
      
      let lR = radius - 18 * scale
      let lp = CGPoint(x: center.x + cosA * lR, y: center.y + sinA * lR)
      context.translateBy(x: lp.x, y: lp.y)
      context.rotate(by: .radians(angRad + .pi/2))
      context.draw(Text("\(p)").font(vintageFont).foregroundColor(.primary.opacity(0.3)), at: .zero, anchor: .center)
      context.rotate(by: .radians(-(angRad + .pi/2)))
      context.translateBy(x: -lp.x, y: -lp.y)
    }

    // 5. Red Zone Arc
    let redS = angleForVU(0)
    context.stroke(Path { p in p.addArc(center: center, radius: radius + 2 * scale, startAngle: .degrees(redS), endAngle: .degrees(endAngle), clockwise: false) }, with: .color(.red.opacity(0.8)), lineWidth: 4 * scale)

    // 6. Perfected Needle
    let currentVU = level - refLevel
    let nAng = angleForVU(currentVU) * .pi / 180
    let nR = radius + params.needleExtension * scale
    let ne = CGPoint(x: center.x + cos(nAng) * nR, y: center.y + sin(nAng) * nR)
    context.stroke(Path { p in p.move(to: center); p.addLine(to: ne) }, with: .color(.primary.opacity(0.9)), lineWidth: 1.2 * scale)
    
    // 7. Glass Surface Reflection
    let glass = Gradient(colors: [.white.opacity(0.25), .clear, .black.opacity(0.05)])
    context.fill(Path(CGRect(origin: .zero, size: size)), with: .linearGradient(glass, startPoint: .zero, endPoint: CGPoint(x: w, y: h)))
    
    // 8. ADDITIVE LIGHT WASH
    context.fill(Path(CGRect(origin: .zero, size: size)), with: .color(bulbAmberColor.opacity(params.lightWash)))
  }
}

// MARK: - Analog VU Card (Dashboard)

struct AnalogVUCard: View {
    @EnvironmentObject var levels: LevelState
    
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Analog VU").font(.headline)
            HStack(spacing: 24) {
                AnalogVUMeter(level: levels.playbackRms.left, label: "LEFT")
                AnalogVUMeter(level: levels.playbackRms.right, label: "RIGHT")
            }
        }
        .padding()
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
    }
}
