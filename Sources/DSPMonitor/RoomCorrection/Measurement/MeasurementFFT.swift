// Self-contained Double-precision Real FFT wrapper for Measurement module.
// Wraps Apple's C `vDSP_fft_zripD` for double-precision support.

import Accelerate
import Foundation

internal enum MeasurementFFTError: Error, CustomStringConvertible {
  case invalidLength(String)
  case setupFailed

  var description: String {
    switch self {
    case .invalidLength(let msg): return "MeasurementFFT length error: \(msg)"
    case .setupFailed: return "MeasurementFFT: failed to create FFT setup (vDSP_create_fftsetupD failed)"
    }
  }
}

internal final class MeasurementFFT {
  internal let length: Int
  private let halfN: Int
  private let log2n: vDSP_Length
  private let setup: FFTSetupD

  init(length: Int) throws {
    guard length >= 8 && length.nonzeroBitCount == 1 else {
      throw MeasurementFFTError.invalidLength("length must be a power of two >= 8, got \(length)")
    }
    self.length = length
    self.halfN = length / 2
    let log2nVal = vDSP_Length(length.trailingZeroBitCount)
    guard let fftSetup = vDSP_create_fftsetupD(log2nVal, FFTRadix(kFFTRadix2)) else {
      throw MeasurementFFTError.setupFailed
    }
    self.setup = fftSetup
    self.log2n = log2nVal
  }

  deinit {
    vDSP_destroy_fftsetupD(setup)
  }

  func forward(
    realIn: UnsafePointer<Double>,
    specRe: UnsafeMutablePointer<Double>,
    specIm: UnsafeMutablePointer<Double>
  ) {
    let n = halfN
    let scratch = UnsafeMutablePointer<Double>.allocate(capacity: n * 2)
    defer { scratch.deallocate() }

    let rePtr = scratch
    let imPtr = scratch + n
    var split = DSPDoubleSplitComplex(realp: rePtr, imagp: imPtr)

    realIn.withMemoryRebound(to: DSPDoubleComplex.self, capacity: n) { complexIn in
      vDSP_ctozD(complexIn, 2, &split, 1, vDSP_Length(n))
    }

    vDSP_fft_zripD(setup, &split, 1, log2n, FFTDirection(kFFTDirection_Forward))

    let reBuf = UnsafeBufferPointer(start: rePtr, count: n)
    var specReBuf = UnsafeMutableBufferPointer(start: specRe, count: n)
    vDSP.multiply(0.5, reBuf, result: &specReBuf)

    if n > 1 {
      let imBuf = UnsafeBufferPointer(start: imPtr + 1, count: n - 1)
      var specImBuf = UnsafeMutableBufferPointer(start: specIm + 1, count: n - 1)
      vDSP.multiply(0.5, imBuf, result: &specImBuf)
    }

    specIm[0] = 0
    specRe[n] = imPtr[0] * 0.5
    specIm[n] = 0
  }

  func inverse(
    specRe: UnsafePointer<Double>,
    specIm: UnsafePointer<Double>,
    realOut: UnsafeMutablePointer<Double>
  ) {
    let n = halfN
    let scratch = UnsafeMutablePointer<Double>.allocate(capacity: n * 2)
    defer { scratch.deallocate() }

    let rePtr = scratch
    let imPtr = scratch + n

    rePtr[0] = specRe[0]
    imPtr[0] = specRe[n]
    if n > 1 {
      (rePtr + 1).update(from: specRe + 1, count: n - 1)
      (imPtr + 1).update(from: specIm + 1, count: n - 1)
    }

    var split = DSPDoubleSplitComplex(realp: rePtr, imagp: imPtr)
    vDSP_fft_zripD(setup, &split, 1, log2n, FFTDirection(kFFTDirection_Inverse))

    realOut.withMemoryRebound(to: DSPDoubleComplex.self, capacity: n) { complexOut in
      vDSP_ztocD(&split, 1, complexOut, 2, vDSP_Length(n))
    }
  }
}
