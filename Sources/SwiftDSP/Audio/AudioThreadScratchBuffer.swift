import Foundation

/// A safe RAII wrapper for UnsafeMutablePointer to automate allocation
/// and deallocation of Double scratch buffers used on the audio thread.
public final class AudioThreadScratchBuffer {
  public let pointer: UnsafeMutablePointer<Double>
  public let capacity: Int

  public init(capacity: Int, repeating: Double = 0.0) {
    self.capacity = capacity
    self.pointer = UnsafeMutablePointer<Double>.allocate(capacity: capacity)
    self.pointer.initialize(repeating: repeating, count: capacity)
  }

  public init(copying array: [Double]) {
    self.capacity = array.count
    self.pointer = UnsafeMutablePointer<Double>.allocate(capacity: array.count)
    array.withUnsafeBufferPointer { buffer in
      if let base = buffer.baseAddress {
        self.pointer.initialize(from: base, count: array.count)
      } else {
        self.pointer.initialize(repeating: 0.0, count: array.count)
      }
    }
  }

  deinit {
    pointer.deinitialize(count: capacity)
    pointer.deallocate()
  }

  @inlinable
  public subscript(index: Int) -> Double {
    @inline(__always) get { pointer[index] }
    @inline(__always) set { pointer[index] = newValue }
  }

  @inlinable
  public func update(from source: AudioThreadScratchBuffer) {
    precondition(self.capacity == source.capacity, "Buffer capacity mismatch")
    self.pointer.update(from: source.pointer, count: capacity)
  }
}

extension UnsafeMutablePointer where Pointee == Double {
  @inlinable
  public func update(from scratch: AudioThreadScratchBuffer, count: Int) {
    self.update(from: scratch.pointer, count: count)
  }
}

extension AudioChunk {
  /// Sums multiple channels of a chunk into a single destination buffer.
  @inlinable
  public func sumChannels(
    _ channels: [Int],
    into dest: AudioThreadScratchBuffer,
    count: Int
  ) {
    self.sumChannels(channels, into: dest.pointer, count: count)
  }

  /// Applies sample-by-sample linear gains to multiple channels.
  @inlinable
  public func applyGain(
    to channels: [Int],
    from gainMultipliers: AudioThreadScratchBuffer,
    count: Int
  ) {
    self.applyGain(to: channels, from: gainMultipliers.pointer, count: count)
  }
}

extension DSPOps {
  /// Element-wise vector multiplication: `b[i] *= a[i]` for `i < count`.
  @inlinable
  public static func multiply(
    _ a: AudioThreadScratchBuffer,
    _ b: MutableWaveform,
    count: Int
  ) {
    self.multiply(a.pointer, b, count: count)
  }
}
