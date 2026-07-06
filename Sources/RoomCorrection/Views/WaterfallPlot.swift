// WaterfallPlot — Cumulative Spectral Decay (CSD) visualization.
//
// Renders time-frequency decay of room modes using an isometric
// layered path renderer. Slices are computed via the `stft` utility
// on the background thread to maintain high UI responsiveness.

import DSPMeasurement
import SwiftUI

struct WaterfallPlot: View {
  @Environment(MeasurementSession.self) var session

  @State private var sliceCount: Int = 30
  @State private var maxTimeMs: Double = 400.0
  @State private var windowLength: Int = 2048

  @State private var slices: [(time: Double, response: FrequencyResponse)] = []
  @State private var isComputing: Bool = false

  // Display parameters
  @State private var fMin: Double = 20.0
  @State private var fMax: Double = 1_000.0
  @State private var floorDB: Double = -40.0

  var body: some View {
    VStack(spacing: 12) {
      controlsStrip

      GeometryReader { geo in
        ZStack {
          Canvas { ctx, size in
            guard !slices.isEmpty else { return }

            // Perspective offsets per slice
            let totalDepthY = size.height * 0.4
            let totalShiftX = size.width * 0.15
            let plotWidth = size.width - totalShiftX
            let plotHeight = size.height - totalDepthY

            // Compute grid points mapping
            let logMin = log10(fMin)
            let logMax = log10(fMax)
            let dLog = logMax - logMin

            // Reference level (first slice peak)
            let refPeak =
              slices.first?.response.real.indices.map {
                slices[0].response.magnitude(at: $0)
              }.max() ?? 1.0
            let refDB = refPeak > 0 ? 20.0 * log10(refPeak) : 0.0

            // Draw back-to-front for correct isometric occlusion
            for (idx, slice) in slices.enumerated().reversed() {
              let progress = Double(idx) / Double(max(1, slices.count - 1))
              let shiftX = totalShiftX * progress
              let shiftY = totalDepthY * progress

              var path = Path()
              var isFirst = true

              let fr = slice.response
              let count = fr.bins

              // Subsample dense bins for performant Canvas rendering
              let binStride = max(1, count / 800)

              for bin in stride(from: 0, to: count, by: binStride) {
                let f = fr.frequency(at: bin)
                guard f >= fMin && f <= fMax else { continue }

                let mag = fr.magnitude(at: bin)
                let db = mag > 0 ? 20.0 * log10(mag) - refDB : -100.0
                let clampedDB = max(floorDB, min(10.0, db))

                // Map frequency to X log scale
                let xFrac = (log10(f) - logMin) / dLog
                let x = shiftX + xFrac * plotWidth

                // Map magnitude to Y linear scale
                let yFrac = (clampedDB - floorDB) / (10.0 - floorDB)
                let y = size.height - shiftY - yFrac * plotHeight

                if isFirst {
                  path.move(to: CGPoint(x: x, y: y))
                  isFirst = false
                } else {
                  path.addLine(to: CGPoint(x: x, y: y))
                }
              }

              // Close path to bottom to allow background masking fill
              if !path.isEmpty {
                var fillPath = path
                fillPath.addLine(to: CGPoint(x: shiftX + plotWidth, y: size.height - shiftY))
                fillPath.addLine(to: CGPoint(x: shiftX, y: size.height - shiftY))
                fillPath.closeSubpath()

                // Masking fill behind the slice line
                ctx.fill(
                  fillPath,
                  with: .color(Color(nsColor: .controlBackgroundColor).opacity(0.95)))

                // Color map based on slice age
                let hue = 0.6 - 0.5 * (1.0 - progress)
                let sliceColor = Color(hue: hue, saturation: 0.8, brightness: 0.9)

                ctx.stroke(path, with: .color(sliceColor), style: StrokeStyle(lineWidth: 1.5))
              }
            }
          }

          if isComputing {
            ProgressView()
              .controlSize(.large)
          } else if slices.isEmpty {
            Text("No measurement data available to generate Waterfall.")
              .font(.callout)
              .foregroundStyle(.secondary)
          }
        }
      }
      .background(Color(nsColor: .controlBackgroundColor))
      .cornerRadius(8)
      .overlay(
        RoundedRectangle(cornerRadius: 8)
          .stroke(Color(nsColor: .separatorColor), lineWidth: 1)
      )
    }
    .padding(.vertical, 4)
    .onAppear { computeSlices() }
    .onChange(of: maxTimeMs) { _, _ in computeSlices() }
    .onChange(of: sliceCount) { _, _ in computeSlices() }
    .onChange(of: windowLength) { _, _ in computeSlices() }
  }

  private var controlsStrip: some View {
    HStack(spacing: 16) {
      HStack(spacing: 6) {
        Text("Time Range:")
          .font(.caption)
          .foregroundStyle(.secondary)
        Picker("", selection: $maxTimeMs) {
          Text("200 ms").tag(200.0)
          Text("400 ms").tag(400.0)
          Text("600 ms").tag(600.0)
          Text("1000 ms").tag(1000.0)
        }
        .pickerStyle(.menu)
        .labelsHidden()
        .frame(width: 90)
      }

      HStack(spacing: 6) {
        Text("Slices:")
          .font(.caption)
          .foregroundStyle(.secondary)
        Picker("", selection: $sliceCount) {
          Text("20").tag(20)
          Text("30").tag(30)
          Text("40").tag(40)
          Text("60").tag(60)
        }
        .pickerStyle(.menu)
        .labelsHidden()
        .frame(width: 70)
      }

      HStack(spacing: 6) {
        Text("Window:")
          .font(.caption)
          .foregroundStyle(.secondary)
        Picker("", selection: $windowLength) {
          Text("1024").tag(1024)
          Text("2048").tag(2048)
          Text("4096").tag(4096)
        }
        .pickerStyle(.menu)
        .labelsHidden()
        .frame(width: 80)
      }

      Spacer()

      HStack(spacing: 6) {
        Text("Floor:")
          .font(.caption)
          .foregroundStyle(.secondary)
        Picker("", selection: $floorDB) {
          Text("-30 dB").tag(-30.0)
          Text("-40 dB").tag(-40.0)
          Text("-50 dB").tag(-50.0)
          Text("-60 dB").tag(-60.0)
        }
        .pickerStyle(.menu)
        .labelsHidden()
        .frame(width: 80)
      }
    }
  }

  private func computeSlices() {
    guard let ir = session.measuredIR else {
      slices = []
      return
    }
    isComputing = true
    let count = sliceCount
    let tMax = maxTimeMs / 1000.0
    let wLen = windowLength
    // Make FFT size twice window length for smooth bins
    let nFft = wLen * 2

    Task.detached(priority: .userInitiated) {
      let res = FrequencyResponse.stft(
        impulseResponse: ir,
        sliceCount: count,
        maxTimeSeconds: tMax,
        windowLength: wLen,
        fftSize: nFft
      )
      await MainActor.run {
        self.slices = res
        self.isComputing = false
      }
    }
  }
}
