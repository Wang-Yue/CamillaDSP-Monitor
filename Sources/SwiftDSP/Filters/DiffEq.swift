import DSPConfig
import Foundation

final class DiffEqFilter: Filter {
  let name: String
  private var x: UnsafeMutablePointer<Double>
  private var y: UnsafeMutablePointer<Double>
  private var a: UnsafePointer<Double>
  private var b: UnsafePointer<Double>
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

    let aPtr = UnsafeMutablePointer<Double>.allocate(capacity: aCoeffs.count)
    aPtr.initialize(from: aCoeffs, count: aCoeffs.count)
    self.a = UnsafePointer(aPtr)

    let bPtr = UnsafeMutablePointer<Double>.allocate(capacity: bCoeffs.count)
    bPtr.initialize(from: bCoeffs, count: bCoeffs.count)
    self.b = UnsafePointer(bPtr)

    self.x = .allocate(capacity: bCoeffs.count)
    self.x.initialize(repeating: 0.0, count: bCoeffs.count)

    self.y = .allocate(capacity: aCoeffs.count)
    self.y.initialize(repeating: 0.0, count: aCoeffs.count)

    self.idxX = 0
    self.idxY = 0
  }

  deinit {
    UnsafeMutablePointer(mutating: a).deallocate()
    UnsafeMutablePointer(mutating: b).deallocate()
    x.deallocate()
    y.deallocate()
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

  func updateParameters(_ config: FilterConfig, sampleRate: Int) {
    guard case .diffEq(let params) = config else { return }
    var aCoeffs = params.a ?? [1.0]
    var bCoeffs = params.b ?? [1.0]
    if aCoeffs.isEmpty { aCoeffs = [1.0] }
    if bCoeffs.isEmpty { bCoeffs = [1.0] }

    UnsafeMutablePointer(mutating: self.a).deallocate()
    UnsafeMutablePointer(mutating: self.b).deallocate()

    let oldACount = self.aCount
    let oldBCount = self.bCount

    self.aCount = aCoeffs.count
    self.bCount = bCoeffs.count

    let aPtr = UnsafeMutablePointer<Double>.allocate(capacity: aCoeffs.count)
    aPtr.initialize(from: aCoeffs, count: aCoeffs.count)
    self.a = UnsafePointer(aPtr)

    let bPtr = UnsafeMutablePointer<Double>.allocate(capacity: bCoeffs.count)
    bPtr.initialize(from: bCoeffs, count: bCoeffs.count)
    self.b = UnsafePointer(bPtr)

    if oldBCount != bCoeffs.count {
      self.x.deallocate()
      self.x = .allocate(capacity: bCoeffs.count)
      self.x.initialize(repeating: 0.0, count: bCoeffs.count)
      self.idxX = 0
    }
    if oldACount != aCoeffs.count {
      self.y.deallocate()
      self.y = .allocate(capacity: aCoeffs.count)
      self.y.initialize(repeating: 0.0, count: aCoeffs.count)
      self.idxY = 0
    }
  }
}
