import Foundation
import SwiftUI

struct LogRangeSlider: View {
  @Binding var minValue: Double
  @Binding var maxValue: Double
  let range: ClosedRange<Double>

  var body: some View {
    GeometryReader { geometry in
      let width = geometry.size.width

      let logMin = log10(range.lowerBound)
      let logMax = log10(range.upperBound)

      let normMin = (log10(minValue) - logMin) / (logMax - logMin)
      let normMax = (log10(maxValue) - logMin) / (logMax - logMin)

      let minX = CGFloat(normMin) * width
      let maxX = CGFloat(normMax) * width

      ZStack(alignment: .leading) {
        // Track
        Rectangle()
          .fill(Color.secondary.opacity(0.2))
          .frame(height: 4)
          .cornerRadius(2)

        // Highlighted track
        Rectangle()
          .fill(Color.accentColor)
          .frame(width: max(0, maxX - minX), height: 4)
          .cornerRadius(2)
          .position(x: (minX + maxX) / 2, y: 10)

        // Min Knob
        Circle()
          .fill(Color.white)
          .frame(width: 16, height: 16)
          .shadow(radius: 2)
          .offset(x: minX - 8)
          .gesture(
            DragGesture()
              .onChanged { value in
                let newX = max(0, min(value.location.x, maxX - 16))
                let newNorm = Double(newX / width)
                let newLog = logMin + newNorm * (logMax - logMin)
                let newValue = pow(10, newLog)
                // Round to nearest integer for frequency
                minValue = round(newValue)
              }
          )

        // Max Knob
        Circle()
          .fill(Color.white)
          .frame(width: 16, height: 16)
          .shadow(radius: 2)
          .offset(x: maxX - 8)
          .gesture(
            DragGesture()
              .onChanged { value in
                let newX = min(width, max(value.location.x, minX + 16))
                let newNorm = Double(newX / width)
                let newLog = logMin + newNorm * (logMax - logMin)
                let newValue = pow(10, newLog)
                // Round to nearest integer for frequency
                maxValue = round(newValue)
              }
          )
      }
      .frame(maxHeight: .infinity)
    }
    .frame(height: 20)
  }
}
