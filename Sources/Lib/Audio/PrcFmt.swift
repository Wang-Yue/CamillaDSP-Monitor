// Internal processing precision type
// Default is Double (f64). Change to Float for 32-bit processing.

import Accelerate

/// Internal processing precision type. All audio math uses this type.
public typealias PrcFmt = Double

/// A high-performance descriptive view of a single channel's mutable buffer pointer
public typealias MutableWaveform = UnsafeMutableBufferPointer<PrcFmt>

/// A high-performance descriptive view of a single channel's buffer pointer
public typealias Waveform = UnsafeBufferPointer<PrcFmt>

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

  // MARK: - Pointer-based hot-path overloads
  //
  // These accept `UnsafeBufferPointer` / `UnsafeMutableBufferPointer` directly
  // so callers holding stable pointers (e.g. an `AudioBuffers` channel view)
  // can avoid Swift's `Array.withUnsafeMutableBufferPointer` uniqueness check
  // — the operation that historically triggered Copy-On-Write allocations on
  // the audio thread.

  /// In-place multiply: `buffer[i] *= scalar` for `i < buffer.count`.
  @inlinable
  public static func scalarMultiply(
    _ buffer: MutableWaveform, by scalar: PrcFmt
  ) {
    var b = buffer
    vDSP.multiply(scalar, b, result: &b)
  }

  /// Zero `buffer.count` samples in-place.
  @inlinable
  public static func clear(_ buffer: MutableWaveform) {
    var b = buffer
    vDSP.clear(&b)
  }

  /// `b += a` over the first `count` samples (must satisfy
  /// `count <= a.count` and `count <= b.count`).
  @inlinable
  public static func add(
    _ a: Waveform,
    _ b: MutableWaveform,
    count: Int
  ) {
    let aSub = UnsafeBufferPointer(start: a.baseAddress, count: count)
    var bSub = UnsafeMutableBufferPointer(start: b.baseAddress, count: count)
    vDSP.add(aSub, bSub, result: &bSub)
  }

  /// `accumulator += a * scalar` over the first `count` samples.
  @inlinable
  public static func multiplyAdd(
    _ a: Waveform,
    _ scalar: PrcFmt,
    accumulator: MutableWaveform,
    count: Int
  ) {
    let aSub = UnsafeBufferPointer(start: a.baseAddress, count: count)
    var accSub = UnsafeMutableBufferPointer(start: accumulator.baseAddress, count: count)
    vDSP.add(multiplication: (a: aSub, b: scalar), accSub, result: &accSub)
  }

  /// Peak absolute value across the first `count` samples.
  @inlinable
  public static func peakAbsolute(
    _ buffer: Waveform, count: Int
  ) -> PrcFmt {
    vDSP.maximumMagnitude(UnsafeBufferPointer(start: buffer.baseAddress, count: count))
  }

  /// RMS over the first `count` samples.
  @inlinable
  public static func rms(
    _ buffer: Waveform, count: Int
  ) -> PrcFmt {
    vDSP.rootMeanSquare(UnsafeBufferPointer(start: buffer.baseAddress, count: count))
  }

  /// Element-wise vector multiplication: `b[i] *= a[i]` for `i < count`.
  @inlinable
  public static func multiply(
    _ a: [PrcFmt],
    _ b: MutableWaveform,
    count: Int
  ) {
    a.withUnsafeBufferPointer { aPtr in
      let aSub = UnsafeBufferPointer(start: aPtr.baseAddress, count: count)
      var bSub = UnsafeMutableBufferPointer(start: b.baseAddress, count: count)
      vDSP.multiply(aSub, bSub, result: &bSub)
    }
  }
}
