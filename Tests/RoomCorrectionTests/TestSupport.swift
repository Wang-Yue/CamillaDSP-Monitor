import Foundation

@testable import SwiftDSP

extension Filter {
  /// Test-only adapter — the library API takes a buffer pointer (no CoW),
  /// but tests find `[Double]` literals more convenient.
  func process(waveform: inout [Double]) {
    waveform.withUnsafeMutableBufferPointer { ptr in
      process(waveform: ptr)
    }
  }
}
