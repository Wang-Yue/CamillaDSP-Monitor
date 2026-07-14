import Observation
import SwiftUI

// MARK: - VU Calibration Settings (Persistent via UserDefaults)

enum VUTheme: String, CaseIterable, Identifiable {
  case vintage = "Vintage Amber"
  case stealth = "Dark Stealth"
  case warmTube = "Warm Tube"

  var id: String { self.rawValue }
}

@Observable
class VUSettings {
  private let defaults = UserDefaults.standard

  var radiusScale: Double {
    didSet { defaults.set(radiusScale, forKey: "vu_radius_scale") }
  }
  var pivotY: Double {
    didSet { defaults.set(pivotY, forKey: "vu_pivot_y") }
  }
  var needleExtension: Double {
    didSet { defaults.set(needleExtension, forKey: "vu_needle_extension") }
  }
  var ambientGlow: Double {
    didSet { defaults.set(ambientGlow, forKey: "vu_ambient_glow") }
  }
  var hotSpotAlpha: Double {
    didSet { defaults.set(hotSpotAlpha, forKey: "vu_hot_spot_alpha") }
  }
  var lightWash: Double {
    didSet { defaults.set(lightWash, forKey: "vu_light_wash") }
  }
  var theme: VUTheme {
    didSet { defaults.set(theme.rawValue, forKey: "vu_theme") }
  }

  init() {
    radiusScale = UserDefaults.standard.object(forKey: "vu_radius_scale") as? Double ?? 1.20
    pivotY = UserDefaults.standard.object(forKey: "vu_pivot_y") as? Double ?? 1.55
    needleExtension = UserDefaults.standard.object(forKey: "vu_needle_extension") as? Double ?? 45.0
    ambientGlow = UserDefaults.standard.object(forKey: "vu_ambient_glow") as? Double ?? 0.5
    hotSpotAlpha = UserDefaults.standard.object(forKey: "vu_hot_spot_alpha") as? Double ?? 0.5
    lightWash = UserDefaults.standard.object(forKey: "vu_light_wash") as? Double ?? 0.2

    if let themeStr = UserDefaults.standard.string(forKey: "vu_theme"),
      let loadedTheme = VUTheme(rawValue: themeStr)
    {
      theme = loadedTheme
    } else {
      theme = .vintage
    }
  }

  func reset() {
    radiusScale = 1.20
    pivotY = 1.55
    needleExtension = 45.0
    ambientGlow = 0.5
    hotSpotAlpha = 0.5
    lightWash = 0.2
    theme = .vintage
  }

  var params: VUParams {
    VUParams(
      radiusScale: radiusScale,
      pivotY: pivotY,
      needleExtension: needleExtension,
      ambientGlow: ambientGlow,
      hotSpotAlpha: hotSpotAlpha,
      lightWash: lightWash,
      theme: theme
    )
  }
}

struct VUParams {
  var radiusScale: Double
  var pivotY: Double
  var needleExtension: Double
  var ambientGlow: Double
  var hotSpotAlpha: Double
  var lightWash: Double
  var theme: VUTheme
}

// MARK: - Hyper-Realistic VU Meter

struct AnalogVUMeter: View {
  var isPlayback: Bool = true
  let channelIndex: Int
  let label: String
  var params: VUParams

  @Environment(LevelState.self) var levels
  private let refLevel = -18.0

  private var bulbAmberColor: Color {
    switch params.theme {
    case .vintage: return Color(red: 1.0, green: 0.82, blue: 0.40)
    case .stealth: return Color.black.opacity(0.4)
    case .warmTube: return Color(red: 0.95, green: 0.45, blue: 0.1)
    }
  }

  private var bulbHotSpotColor: Color {
    switch params.theme {
    case .vintage: return Color(red: 1.0, green: 0.98, blue: 0.88)
    case .stealth: return Color.white.opacity(0.15)
    case .warmTube: return Color(red: 1.0, green: 0.8, blue: 0.3)
    }
  }

  private var needleColor: Color {
    switch params.theme {
    case .vintage: return .primary.opacity(0.9)
    case .stealth: return .white
    case .warmTube: return Color(red: 0.15, green: 0.15, blue: 0.15)
    }
  }

  private var arcColor: Color {
    switch params.theme {
    case .vintage: return .primary.opacity(0.6)
    case .stealth: return .primary.opacity(0.3)
    case .warmTube: return .primary.opacity(0.5)
    }
  }

  private var percentageMarksColor: Color {
    switch params.theme {
    case .vintage: return .primary.opacity(0.4)
    case .stealth: return .primary.opacity(0.2)
    case .warmTube: return .primary.opacity(0.3)
    }
  }

  private var redZoneColor: Color {
    switch params.theme {
    case .vintage: return .red.opacity(0.8)
    case .stealth: return .primary.opacity(0.5)
    case .warmTube: return Color(red: 0.85, green: 0.2, blue: 0.1).opacity(0.8)
    }
  }

  var body: some View {
    let level =
      isPlayback
      ? (levels.playbackRms.indices.contains(channelIndex)
        ? levels.playbackRms[channelIndex] : -100.0)
      : (levels.captureRms.indices.contains(channelIndex)
        ? levels.captureRms[channelIndex] : -100.0)

    GeometryReader { geometry in
      let h = geometry.size.height
      let scale = h / 160.0
      let vintageFont = Font.custom("Rockwell", size: 10 * scale)

      VStack(spacing: 6 * scale) {
        ZStack {
          AnalogVUMeterDial(
            params: params,
            scale: scale,
            vintageFont: vintageFont,
            bulbAmberColor: bulbAmberColor,
            bulbHotSpotColor: bulbHotSpotColor,
            arcColor: arcColor,
            percentageMarksColor: percentageMarksColor,
            redZoneColor: redZoneColor
          )
          .equatable()

          Canvas { context, size in
            drawNeedle(context: &context, size: size, level: Float(level), scale: scale)
          }
        }
        .frame(maxHeight: .infinity)
        .overlay(
          RoundedRectangle(cornerRadius: 6 * scale).stroke(
            Color.primary.opacity(0.2), lineWidth: 1.2 * scale)
        )
        .clipShape(RoundedRectangle(cornerRadius: 6 * scale))

        Text(label)
          .font(.system(size: 11 * scale, weight: .black))
          .foregroundStyle(.secondary.opacity(0.8))
          .lineLimit(1)
      }
      .padding(4 * scale)
    }
    .aspectRatio(1.6, contentMode: .fit)
  }

  private func drawNeedle(
    context: inout GraphicsContext, size: CGSize, level: Float, scale: CGFloat
  ) {
    let level = Double(level)
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
      let clippedNorm = min(max(norm, -0.076), 1.1)
      return startAngle + clippedNorm * totalSpan
    }

    // 6. Perfected Needle
    let currentVU = level - refLevel
    let nAng = angleForVU(currentVU) * .pi / 180
    let nR = radius + params.needleExtension * scale
    let ne = CGPoint(x: center.x + cos(nAng) * nR, y: center.y + sin(nAng) * nR)
    context.stroke(
      Path { p in
        p.move(to: center)
        p.addLine(to: ne)
      }, with: .color(needleColor), lineWidth: 1.2 * scale)

  }
}

private struct AnalogVUMeterDial: View, Equatable {
  var params: VUParams
  let scale: CGFloat
  let vintageFont: Font

  let bulbAmberColor: Color
  let bulbHotSpotColor: Color
  let arcColor: Color
  let percentageMarksColor: Color
  let redZoneColor: Color

  private let vuMarks: [(v: Double, l: String?)] = [
    (-20, "20"), (-10, "10"), (-7, "7"), (-5, "5"), (-3, "3"), (-2, "2"), (-1, "1"), (0, "0"),
    (1, "1"), (2, "2"), (3, "3"),
  ]

  nonisolated static func == (lhs: AnalogVUMeterDial, rhs: AnalogVUMeterDial) -> Bool {
    lhs.scale == rhs.scale && lhs.params.radiusScale == rhs.params.radiusScale
      && lhs.params.pivotY == rhs.params.pivotY
      && lhs.params.needleExtension == rhs.params.needleExtension
      && lhs.params.ambientGlow == rhs.params.ambientGlow
      && lhs.params.hotSpotAlpha == rhs.params.hotSpotAlpha
      && lhs.params.lightWash == rhs.params.lightWash && lhs.params.theme == rhs.params.theme
  }

  var body: some View {
    Canvas(
      renderer: { context, size in
        drawVUDial(context: &context, size: size)
      },
      symbols: {
        ForEach(0..<vuMarks.count, id: \.self) { i in
          if let text = vuMarks[i].l {
            let color = vuMarks[i].v >= 0 ? redZoneColor : arcColor
            Text(text)
              .font(vintageFont)
              .foregroundColor(color.opacity(0.6))
              .tag(i)
          }
        }
        ForEach([0, 20, 40, 60, 80, 100], id: \.self) { p in
          Text("\(p)")
            .font(vintageFont)
            .foregroundColor(percentageMarksColor.opacity(0.4))
            .tag(1000 + p)
        }
      }
    )
  }

  private func drawVUDial(context: inout GraphicsContext, size: CGSize) {
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
      let clippedNorm = min(max(norm, -0.076), 1.1)
      return startAngle + clippedNorm * totalSpan
    }

    // 1. BOTTOM AMBER GLOW
    let amberGlow = GraphicsContext.Shading.radialGradient(
      Gradient(stops: [
        .init(color: bulbAmberColor.opacity(params.ambientGlow), location: 0.0),
        .init(color: bulbAmberColor.opacity(0.0), location: 0.8),
      ]),
      center: CGPoint(x: w / 2, y: h + 10 * scale), startRadius: 0, endRadius: h * 1.6
    )
    context.fill(Path(CGRect(origin: .zero, size: size)), with: amberGlow)

    // 2. HOT SPOT
    let hotSpot = GraphicsContext.Shading.radialGradient(
      Gradient(stops: [
        .init(color: bulbHotSpotColor.opacity(params.hotSpotAlpha), location: 0.0),
        .init(color: bulbHotSpotColor.opacity(0.0), location: 1.0),
      ]),
      center: CGPoint(x: w / 2, y: h + 5 * scale),
      startRadius: 0,
      endRadius: h * 0.4
    )
    context.fill(Path(CGRect(origin: .zero, size: size)), with: hotSpot)

    // 3. Main Arc
    context.stroke(
      Path { p in
        p.addArc(
          center: center, radius: radius, startAngle: .degrees(startAngle),
          endAngle: .degrees(endAngle), clockwise: false)
      }, with: .color(arcColor), lineWidth: 1.8 * scale)

    // 4. Marks Drawing
    var regularMarksPath = Path()
    var redMarksPath = Path()

    for m in vuMarks {
      let angDeg = angleForVU(m.v)
      let angRad = angDeg * .pi / 180

      let cosA = cos(angRad)
      let sinA = sin(angRad)
      let s = CGPoint(x: center.x + cosA * radius, y: center.y + sinA * radius)
      let eR = radius + 7 * scale
      let e = CGPoint(x: center.x + cosA * eR, y: center.y + sinA * eR)

      if m.v >= 0 {
        redMarksPath.move(to: s)
        redMarksPath.addLine(to: e)
      } else {
        regularMarksPath.move(to: s)
        regularMarksPath.addLine(to: e)
      }
    }

    context.stroke(regularMarksPath, with: .color(arcColor.opacity(0.7)), lineWidth: 1.8 * scale)
    context.stroke(redMarksPath, with: .color(redZoneColor.opacity(0.7)), lineWidth: 1.8 * scale)

    for (i, m) in vuMarks.enumerated() {
      if m.l != nil {
        let angDeg = angleForVU(m.v)
        let angRad = angDeg * .pi / 180
        let cosA = cos(angRad)
        let sinA = sin(angRad)
        let lR = radius + 18 * scale
        let lp = CGPoint(x: center.x + cosA * lR, y: center.y + sinA * lR)
        context.translateBy(x: lp.x, y: lp.y)
        context.rotate(by: .radians(angRad + .pi / 2))

        if let symbol = context.resolveSymbol(id: i) {
          context.draw(symbol, at: .zero, anchor: .center)
        }

        context.rotate(by: .radians(-(angRad + .pi / 2)))
        context.translateBy(x: -lp.x, y: -lp.y)
      }
    }

    // Percentage Markings (BELOW)
    var percentageMarksPath = Path()
    for p in [0, 20, 40, 60, 80, 100] {
      let ratio = Double(p) / 100.0
      let norm = (ratio - 0.1) / (1.412 - 0.1)
      let angRad = (startAngle + norm * totalSpan) * .pi / 180

      let cosA = cos(angRad)
      let sinA = sin(angRad)
      let s = CGPoint(x: center.x + cosA * radius, y: center.y + sinA * radius)
      let eR = radius - 7 * scale
      let e = CGPoint(x: center.x + cosA * eR, y: center.y + sinA * eR)

      percentageMarksPath.move(to: s)
      percentageMarksPath.addLine(to: e)
    }
    context.stroke(
      percentageMarksPath, with: .color(percentageMarksColor.opacity(0.4)), lineWidth: 1.0 * scale)

    for p in [0, 20, 40, 60, 80, 100] {
      let ratio = Double(p) / 100.0
      let norm = (ratio - 0.1) / (1.412 - 0.1)
      let angRad = (startAngle + norm * totalSpan) * .pi / 180

      let cosA = cos(angRad)
      let sinA = sin(angRad)
      let lR = radius - 18 * scale
      let lp = CGPoint(x: center.x + cosA * lR, y: center.y + sinA * lR)
      context.translateBy(x: lp.x, y: lp.y)
      context.rotate(by: .radians(angRad + .pi / 2))

      if let symbol = context.resolveSymbol(id: 1000 + p) {
        context.draw(symbol, at: .zero, anchor: .center)
      }

      context.rotate(by: .radians(-(angRad + .pi / 2)))
      context.translateBy(x: -lp.x, y: -lp.y)
    }

    // 5. Red Zone Arc
    let redS = angleForVU(0)
    context.stroke(
      Path { p in
        p.addArc(
          center: center, radius: radius + 2 * scale, startAngle: .degrees(redS),
          endAngle: .degrees(endAngle), clockwise: false)
      }, with: .color(redZoneColor), lineWidth: 4 * scale)

    // 6. Glass Surface Reflection (moved from drawNeedle)
    let glass = GraphicsContext.Shading.linearGradient(
      Gradient(colors: [.white.opacity(0.25), .clear, .black.opacity(0.05)]), startPoint: .zero,
      endPoint: CGPoint(x: w, y: h))
    context.fill(Path(CGRect(origin: .zero, size: size)), with: glass)

    // 7. ADDITIVE LIGHT WASH (moved from drawNeedle)
    context.fill(
      Path(CGRect(origin: .zero, size: size)),
      with: .color(bulbAmberColor.opacity(params.lightWash)))
  }
}

// MARK: - Analog VU Card (Dashboard)

struct AnalogVUCard: View {
  @Environment(LevelState.self) var levels
  @Environment(VUSettings.self) var vuSettings

  var body: some View {
    @Bindable var vuSettings = vuSettings
    VStack(alignment: .leading, spacing: 12) {
      HStack {
        Text("Analog VU").font(.headline)
        Spacer()
        Picker("", selection: $vuSettings.theme) {
          ForEach(VUTheme.allCases) { theme in
            Text(theme.rawValue).tag(theme)
          }
        }
        .pickerStyle(.segmented)
        .fixedSize()
      }

      let chCount = levels.playbackChannelCount
      if chCount <= 4 {
        // Equal horizontal division for 1-4 channels
        HStack(spacing: 16) {
          ForEach(0..<chCount, id: \.self) { ch in
            AnalogVUMeter(
              isPlayback: true,
              channelIndex: ch,
              label: channelLabel(for: ch, totalCount: chCount),
              params: vuSettings.params
            )
            .frame(maxWidth: .infinity)
          }
        }
      } else {
        // Scrollview for many channels so they don't shrink too much
        ScrollView(.horizontal, showsIndicators: false) {
          HStack(spacing: 16) {
            ForEach(0..<chCount, id: \.self) { ch in
              AnalogVUMeter(
                isPlayback: true,
                channelIndex: ch,
                label: channelLabel(for: ch, totalCount: chCount),
                params: vuSettings.params
              )
              .frame(width: 220)
            }
          }
        }
      }
    }
    .padding()
    .background(Color.primary.opacity(0.05), in: RoundedRectangle(cornerRadius: 12))
    .onAppear { levels.visibilityCount += 1 }
    .onDisappear { levels.visibilityCount -= 1 }
  }

  private func channelLabel(for index: Int, totalCount: Int) -> String {
    "\(index + 1)"
  }
}
