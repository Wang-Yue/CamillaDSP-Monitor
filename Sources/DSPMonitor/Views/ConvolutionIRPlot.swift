// Compact IR mini-plot — shared by `ConvolutionPresetDetailView`
// and the Convolution stage detail's per-channel summary cards.
//
// Loads the IR file off disk on appear and on path-change. The file
// format is the same raw little-endian Double stream that
// `MeasurementSession.persistIR` writes (matches `format: F64_LE`).

import AppKit
import Foundation
import SwiftUI

/// Self-contained IR plot. Pass the absolute path; the view loads the
/// file lazily and renders. Falls back to a one-line error if the
/// file is missing or unreadable.
struct ConvolutionIRPlot: View {
  let irPath: String
  /// Optional title shown above the plot. Used by stage detail to
  /// label "Left" / "Right" cards.
  var title: String? = nil

  @State private var loadedIR: [Double]?
  @State private var loadError: String?

  var body: some View {
    VStack(alignment: .leading, spacing: 4) {
      if let title = title {
        Text(title)
          .font(.caption.bold())
          .foregroundStyle(.secondary)
      }
      Group {
        if let ir = loadedIR {
          IRWaveformCanvas(ir: ir)
        } else if let err = loadError {
          Text(err)
            .font(.caption)
            .foregroundStyle(.secondary)
            .padding(8)
        } else {
          ProgressView().padding()
        }
      }
    }
    .onAppear { loadIR() }
    .onChange(of: irPath) { _, _ in loadIR() }
  }

  private func loadIR() {
    let url = URL(fileURLWithPath: irPath)
    do {
      let data = try Data(contentsOf: url)
      let n = data.count / MemoryLayout<Double>.size
      var samples = [Double](repeating: 0, count: n)
      _ = samples.withUnsafeMutableBytes { dst in
        data.copyBytes(to: dst, count: n * MemoryLayout<Double>.size)
      }
      loadedIR = samples
      loadError = nil
    } catch {
      loadedIR = nil
      loadError = "Could not load IR: \(error.localizedDescription)"
    }
  }
}

/// Pure-drawing view: takes a raw IR sample array and renders the
/// waveform with auto-scaled Y axis.
private struct IRWaveformCanvas: View {
  let ir: [Double]

  var body: some View {
    GeometryReader { geo in
      let w = geo.size.width
      let h = geo.size.height
      ZStack {
        RoundedRectangle(cornerRadius: 6)
          .fill(Color(nsColor: .textBackgroundColor))

        Path { p in
          p.move(to: CGPoint(x: 0, y: h / 2))
          p.addLine(to: CGPoint(x: w, y: h / 2))
        }.stroke(Color.primary.opacity(0.18), lineWidth: 1)

        Path { path in
          let n = ir.count
          if n == 0 { return }
          let peak = max(1e-9, ir.map { abs($0) }.max() ?? 1)
          for i in 0..<n {
            let x = w * Double(i) / Double(max(1, n - 1))
            let y = h * (1.0 - (ir[i] / peak + 1) / 2)
            if i == 0 {
              path.move(to: CGPoint(x: x, y: y))
            } else {
              path.addLine(to: CGPoint(x: x, y: y))
            }
          }
        }
        .stroke(Color.blue, lineWidth: 1.0)
      }
      .clipShape(RoundedRectangle(cornerRadius: 6))
    }
  }
}
