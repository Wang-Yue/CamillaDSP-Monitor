import DSPConfig
import Foundation

final class DiffEqFilter: Filter {
  let name: String
  private var x: AudioThreadScratchBuffer
  private var y: AudioThreadScratchBuffer
  private var a: AudioThreadScratchBuffer
  private var b: AudioThreadScratchBuffer
  private var aCount: Int = 0
  private var bCount: Int = 0
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

    self.aCount = aCoeffs.count
    self.bCount = bCoeffs.count

    self.a = AudioThreadScratchBuffer(copying: aCoeffs)
    self.b = AudioThreadScratchBuffer(copying: bCoeffs)

    self.x = AudioThreadScratchBuffer(capacity: bCoeffs.count, repeating: 0.0)
    self.y = AudioThreadScratchBuffer(capacity: aCoeffs.count, repeating: 0.0)

    self.idxX = 0
    self.idxY = 0
  }

  func process(waveform: MutableWaveform) {
    let nb = bCount
    let na = aCount
    guard let wBase = waveform.baseAddress else { return }

    for i in 0..<waveform.count {
      idxX = (idxX + 1) % nb
      idxY = (idxY + 1) % na
      x[idxX] = wBase[i]

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
      wBase[i] = out
    }
  }
}
