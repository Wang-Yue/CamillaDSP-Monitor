// CamillaDSP-Swift: Internal processing precision type
// Default is Double (f64). Change to Float for 32-bit processing.

import Accelerate

/// Internal processing precision type. All audio math uses this type.
public typealias PrcFmt = Double

/// vDSP length type matching PrcFmt
public typealias PrcFmtLength = vDSP_Length

extension PrcFmt {
  /// Convert dB to linear gain
  @inlinable
  public static func fromDB(_ db: PrcFmt) -> PrcFmt {
    pow(10.0, db / 20.0)
  }

  /// Convert linear gain to dB. Returns -1000.0 for zero/negative input (matches Rust sentinel).
  @inlinable
  public static func toDB(_ linear: PrcFmt) -> PrcFmt {
    if linear <= 0 { return -1000.0 }
    return 20.0 * log10(linear)
  }
}

/// Vectorized DSP operations using Accelerate's Swift `vDSP` namespace.
///
/// The partial-count ops (`add`, `multiply`, `multiplyAdd`) need to
/// operate on the first `count` elements of arrays that may be longer
/// (chunks have a `validFrames` ≤ `frames`). Swift `vDSP.add(_:_:result:)`
/// derives the length from the buffer, so the partial-count ops slice
/// the buffers via `UnsafeBufferPointer(rebasing:)` to give vDSP an
/// exact-length view without copying.
public enum DSPOps {
  /// Multiply vector by scalar in-place: buffer *= scalar
  @inlinable
  public static func scalarMultiply(_ buffer: inout [PrcFmt], by scalar: PrcFmt) {
    buffer.withUnsafeMutableBufferPointer { ptr in
      vDSP.multiply(scalar, ptr, result: &ptr)
    }
  }

  /// Add `a[0..<count]` into `b[0..<count]` (in-place on `b`).
  @inlinable
  public static func add(_ a: [PrcFmt], _ b: inout [PrcFmt], count: Int) {
    a.withUnsafeBufferPointer { aFull in
      b.withUnsafeMutableBufferPointer { bFull in
        let aSub = UnsafeBufferPointer(rebasing: aFull.prefix(count))
        var bSub = UnsafeMutableBufferPointer(rebasing: bFull.prefix(count))
        vDSP.add(aSub, bSub, result: &bSub)
      }
    }
  }

  /// Multiply two vectors element-wise: result[0..<count] = a[0..<count] * b[0..<count]
  @inlinable
  public static func multiply(_ a: [PrcFmt], _ b: [PrcFmt], result: inout [PrcFmt], count: Int) {
    a.withUnsafeBufferPointer { aFull in
      b.withUnsafeBufferPointer { bFull in
        result.withUnsafeMutableBufferPointer { rFull in
          let aSub = UnsafeBufferPointer(rebasing: aFull.prefix(count))
          let bSub = UnsafeBufferPointer(rebasing: bFull.prefix(count))
          var rSub = UnsafeMutableBufferPointer(rebasing: rFull.prefix(count))
          vDSP.multiply(aSub, bSub, result: &rSub)
        }
      }
    }
  }

  /// Multiply-accumulate: accumulator[0..<count] += a[0..<count] * b
  @inlinable
  public static func multiplyAdd(
    _ a: [PrcFmt], _ b: PrcFmt, accumulator: inout [PrcFmt], count: Int
  ) {
    a.withUnsafeBufferPointer { aFull in
      accumulator.withUnsafeMutableBufferPointer { accFull in
        let aSub = UnsafeBufferPointer(rebasing: aFull.prefix(count))
        var accSub = UnsafeMutableBufferPointer(rebasing: accFull.prefix(count))
        // result = (a * b) + accumulator, written into accumulator.
        vDSP.add(multiplication: (a: aSub, b: b), accSub, result: &accSub)
      }
    }
  }

  /// Find peak absolute value across the entire buffer.
  @inlinable
  public static func peakAbsolute(_ buffer: [PrcFmt]) -> PrcFmt {
    vDSP.maximumMagnitude(buffer)
  }

  /// Compute root-mean-square of the entire buffer.
  @inlinable
  public static func rms(_ buffer: [PrcFmt]) -> PrcFmt {
    vDSP.rootMeanSquare(buffer)
  }
}
