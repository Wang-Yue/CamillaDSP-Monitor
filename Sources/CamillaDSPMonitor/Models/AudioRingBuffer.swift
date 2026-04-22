import Foundation
import Synchronization
import Accelerate

/// A lock-free, single-producer, multi-consumer ring buffer
/// optimized for zero-allocation audio processing.
@available(macOS 15.0, *)
final class AudioRingBuffer: @unchecked Sendable {
  private let capacity: Int
  private let mask: Int
  private let buffer: UnsafeMutablePointer<Float>
  private let writePos = Atomic<Int>(0)

  init(capacity: Int) {
    var n = 1
    while n < capacity { n <<= 1 }
    self.capacity = n
    self.mask = n - 1
    
    self.buffer = UnsafeMutablePointer<Float>.allocate(capacity: n)
    self.buffer.initialize(repeating: 0, count: n)
  }

  deinit {
    buffer.deallocate()
  }

  /// Sums planar stereo (or copies mono) data directly into the ring buffer.
  /// Returns the number of samples actually written.
  @discardableResult
  func writeSumming(left: UnsafePointer<Float>, right: UnsafePointer<Float>?, count: Int) -> Int {
    let currentWrite = writePos.load(ordering: .relaxed)
    
    // In a circular buffer used for monitoring, we always allow overwriting.
    // However, we cap the write to the capacity.
    let countToCopy = min(count, capacity)
    guard countToCopy > 0 else { return 0 }

    let startIdx = currentWrite & mask
    let spaceToEnd = capacity - startIdx
    
    if countToCopy <= spaceToEnd {
      sumInto(dst: buffer.advanced(by: startIdx), l: left, r: right, n: countToCopy)
    } else {
      let firstPart = spaceToEnd
      let secondPart = countToCopy - spaceToEnd
      sumInto(dst: buffer.advanced(by: startIdx), l: left, r: right, n: firstPart)
      sumInto(dst: buffer, l: left.advanced(by: firstPart), r: right?.advanced(by: firstPart), n: secondPart)
    }
    
    writePos.store(currentWrite + countToCopy, ordering: .releasing)
    return countToCopy
  }

  private func sumInto(dst: UnsafeMutablePointer<Float>, l: UnsafePointer<Float>, r: UnsafePointer<Float>?, n: Int) {
    if let r = r {
      var scale: Float = 0.5
      vDSP_vadd(l, 1, r, 1, dst, 1, vDSP_Length(n))
      vDSP_vsmul(dst, 1, &scale, dst, 1, vDSP_Length(n))
    } else {
      memcpy(dst, l, n * MemoryLayout<Float>.size)
    }
  }

  /// Reads up to 'count' most recent samples from the buffer.
  /// Returns the number of samples actually read.
  /// This method does NOT advance a read pointer, supporting sliding window analysis.
  func readLatest(count: Int, into outBuffer: UnsafeMutableBufferPointer<Float>) -> Int {
    let currentWrite = writePos.load(ordering: .acquiring)
    
    // Total samples available is capped by capacity
    let availableData = min(currentWrite, capacity)
    let requestedCount = min(count, availableData)
    
    guard requestedCount > 0, let dstBase = outBuffer.baseAddress else { return 0 }
    
    // We want the most recent 'requestedCount' samples, ending at 'currentWrite'
    let startIdx = (currentWrite - requestedCount) & mask
    let spaceToEnd = capacity - startIdx
    
    if requestedCount <= spaceToEnd {
      memcpy(dstBase, buffer.advanced(by: startIdx), requestedCount * MemoryLayout<Float>.size)
    } else {
      let firstPart = spaceToEnd
      let secondPart = requestedCount - spaceToEnd
      memcpy(dstBase, buffer.advanced(by: startIdx), firstPart * MemoryLayout<Float>.size)
      memcpy(dstBase.advanced(by: firstPart), buffer, secondPart * MemoryLayout<Float>.size)
    }
    
    return requestedCount
  }
}
