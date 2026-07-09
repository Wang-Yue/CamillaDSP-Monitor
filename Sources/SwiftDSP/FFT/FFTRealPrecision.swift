import Accelerate
import Foundation

/// Protocol that abstracts Accelerate real FFT setup and operations for Float and Double.
public protocol FFTRealPrecision: BinaryFloatingPoint {
  associatedtype SetupType
  associatedtype SplitComplexType
  associatedtype ComplexType

  static func createSetup(log2n: vDSP_Length, radix: FFTRadix) -> SetupType?
  static func destroySetup(_ setup: SetupType)

  static func ctoz(
    _ complex: UnsafePointer<ComplexType>,
    _ strideComplex: vDSP_Stride,
    _ split: UnsafePointer<SplitComplexType>,
    _ strideSplit: vDSP_Stride,
    _ size: vDSP_Length
  )

  static func ztoc(
    _ split: UnsafePointer<SplitComplexType>,
    _ strideSplit: vDSP_Stride,
    _ complex: UnsafeMutablePointer<ComplexType>,
    _ strideComplex: vDSP_Stride,
    _ size: vDSP_Length
  )

  static func fft_zrip(
    _ setup: SetupType,
    _ split: UnsafePointer<SplitComplexType>,
    _ stride: vDSP_Stride,
    _ log2n: vDSP_Length,
    _ direction: FFTDirection
  )

  static func vsmul(
    _ vector: UnsafePointer<Self>,
    _ strideVector: vDSP_Stride,
    _ scalar: UnsafePointer<Self>,
    _ result: UnsafeMutablePointer<Self>,
    _ strideResult: vDSP_Stride,
    _ size: vDSP_Length
  )

  static func initSplitComplex(realp: UnsafeMutablePointer<Self>, imagp: UnsafeMutablePointer<Self>)
    -> SplitComplexType
}

extension Float: FFTRealPrecision {
  public typealias SetupType = FFTSetup
  public typealias SplitComplexType = DSPSplitComplex
  public typealias ComplexType = DSPComplex

  public static func createSetup(log2n: vDSP_Length, radix: FFTRadix) -> FFTSetup? {
    return vDSP_create_fftsetup(log2n, radix)
  }

  public static func destroySetup(_ setup: FFTSetup) {
    vDSP_destroy_fftsetup(setup)
  }

  public static func ctoz(
    _ complex: UnsafePointer<DSPComplex>,
    _ strideComplex: vDSP_Stride,
    _ split: UnsafePointer<DSPSplitComplex>,
    _ strideSplit: vDSP_Stride,
    _ size: vDSP_Length
  ) {
    vDSP_ctoz(complex, strideComplex, UnsafeMutablePointer(mutating: split), strideSplit, size)
  }

  public static func ztoc(
    _ split: UnsafePointer<DSPSplitComplex>,
    _ strideSplit: vDSP_Stride,
    _ complex: UnsafeMutablePointer<DSPComplex>,
    _ strideComplex: vDSP_Stride,
    _ size: vDSP_Length
  ) {
    vDSP_ztoc(split, strideSplit, complex, strideComplex, size)
  }

  public static func fft_zrip(
    _ setup: FFTSetup,
    _ split: UnsafePointer<DSPSplitComplex>,
    _ stride: vDSP_Stride,
    _ log2n: vDSP_Length,
    _ direction: FFTDirection
  ) {
    vDSP_fft_zrip(setup, UnsafeMutablePointer(mutating: split), stride, log2n, direction)
  }

  public static func vsmul(
    _ vector: UnsafePointer<Float>,
    _ strideVector: vDSP_Stride,
    _ scalar: UnsafePointer<Float>,
    _ result: UnsafeMutablePointer<Float>,
    _ strideResult: vDSP_Stride,
    _ size: vDSP_Length
  ) {
    var nonConstScalar = scalar.pointee
    vDSP_vsmul(vector, strideVector, &nonConstScalar, result, strideResult, size)
  }

  public static func initSplitComplex(
    realp: UnsafeMutablePointer<Float>, imagp: UnsafeMutablePointer<Float>
  ) -> DSPSplitComplex {
    return DSPSplitComplex(realp: realp, imagp: imagp)
  }
}

extension Double: FFTRealPrecision {
  public typealias SetupType = FFTSetupD
  public typealias SplitComplexType = DSPDoubleSplitComplex
  public typealias ComplexType = DSPDoubleComplex

  public static func createSetup(log2n: vDSP_Length, radix: FFTRadix) -> FFTSetupD? {
    return vDSP_create_fftsetupD(log2n, radix)
  }

  public static func destroySetup(_ setup: FFTSetupD) {
    vDSP_destroy_fftsetupD(setup)
  }

  public static func ctoz(
    _ complex: UnsafePointer<DSPDoubleComplex>,
    _ strideComplex: vDSP_Stride,
    _ split: UnsafePointer<DSPDoubleSplitComplex>,
    _ strideSplit: vDSP_Stride,
    _ size: vDSP_Length
  ) {
    vDSP_ctozD(complex, strideComplex, UnsafeMutablePointer(mutating: split), strideSplit, size)
  }

  public static func ztoc(
    _ split: UnsafePointer<DSPDoubleSplitComplex>,
    _ strideSplit: vDSP_Stride,
    _ complex: UnsafeMutablePointer<DSPDoubleComplex>,
    _ strideComplex: vDSP_Stride,
    _ size: vDSP_Length
  ) {
    vDSP_ztocD(split, strideSplit, complex, strideComplex, size)
  }

  public static func fft_zrip(
    _ setup: FFTSetupD,
    _ split: UnsafePointer<DSPDoubleSplitComplex>,
    _ stride: vDSP_Stride,
    _ log2n: vDSP_Length,
    _ direction: FFTDirection
  ) {
    vDSP_fft_zripD(setup, UnsafeMutablePointer(mutating: split), stride, log2n, direction)
  }

  public static func vsmul(
    _ vector: UnsafePointer<Double>,
    _ strideVector: vDSP_Stride,
    _ scalar: UnsafePointer<Double>,
    _ result: UnsafeMutablePointer<Double>,
    _ strideResult: vDSP_Stride,
    _ size: vDSP_Length
  ) {
    var nonConstScalar = scalar.pointee
    vDSP_vsmulD(vector, strideVector, &nonConstScalar, result, strideResult, size)
  }

  public static func initSplitComplex(
    realp: UnsafeMutablePointer<Double>, imagp: UnsafeMutablePointer<Double>
  ) -> DSPDoubleSplitComplex {
    return DSPDoubleSplitComplex(realp: realp, imagp: imagp)
  }
}
