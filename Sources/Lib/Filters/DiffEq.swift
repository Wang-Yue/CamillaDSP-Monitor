import DSPAudio
import DSPConfig
import Foundation

final class DiffEqFilter: Filter {
  let name: String
  private var x: [PrcFmt]
  private var y: [PrcFmt]
  private var a: [PrcFmt]
  private var b: [PrcFmt]
  private var idxX: Int = 0
  private var idxY: Int = 0

  init(name: String = "diffeq", parameters: DiffEqParameters) {
    self.name = name
    var aCoeffs = parameters.a ?? [1.0]
    var bCoeffs = parameters.b ?? [1.0]

    if aCoeffs.isEmpty { aCoeffs = [1.0] }
    if bCoeffs.isEmpty { bCoeffs = [1.0] }

    // Normalize by a[0]
    if let a0 = aCoeffs.first, a0 != 0 && a0 != 1.0 {
      let scale = 1.0 / a0
      aCoeffs = aCoeffs.map { $0 * scale }
      bCoeffs = bCoeffs.map { $0 * scale }
    }

    self.a = aCoeffs
    self.b = bCoeffs
    self.x = [PrcFmt](repeating: 0.0, count: bCoeffs.count)
    self.y = [PrcFmt](repeating: 0.0, count: aCoeffs.count)
    self.idxX = 0
    self.idxY = 0
  }

  func process(waveform: MutableWaveform) {
    let nb = b.count
    let na = a.count

    for i in 0..<waveform.count {
      idxX = (idxX + 1) % nb
      idxY = (idxY + 1) % na
      x[idxX] = waveform[i]

      var out = 0.0
      for n in 0..<nb {
        let nIdx = (idxX + nb - n) % nb
        out += b[n] * x[nIdx]
      }
      for p in 1..<na {
        let pIdx = (idxY + na - p) % na
        out -= a[p] * y[pIdx]
      }
      y[idxY] = out
      waveform[i] = out
    }
    flushSubnormals()
  }

  private func flushSubnormals() {
    for i in 0..<x.count {
      if x[i].isSubnormal {
        x[i] = 0.0
      }
    }
    for i in 0..<y.count {
      if y[i].isSubnormal {
        y[i] = 0.0
      }
    }
  }
  func updateParameters(_ config: FilterConfig, sampleRate: Int) {
    guard case .diffEq(let params) = config else { return }
    var aCoeffs = params.a ?? [1.0]
    var bCoeffs = params.b ?? [1.0]
    if aCoeffs.isEmpty { aCoeffs = [1.0] }
    if bCoeffs.isEmpty { bCoeffs = [1.0] }

    self.a = aCoeffs
    self.b = bCoeffs
    if self.x.count != bCoeffs.count {
      self.x = [PrcFmt](repeating: 0.0, count: bCoeffs.count)
      self.idxX = 0
    }
    if self.y.count != aCoeffs.count {
      self.y = [PrcFmt](repeating: 0.0, count: aCoeffs.count)
      self.idxY = 0
    }
  }
}
