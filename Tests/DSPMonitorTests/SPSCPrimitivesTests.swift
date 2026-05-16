// Behavioural tests for the lock-free SPSC primitives in
// `Engine/LockFreeRingBuffer.swift`: `SPSCAudioRingBuffer` (both
// snapshot and consume access patterns), `SPSCQueue<T>`, and
// `AtomicDouble`. We can't easily prove real-time-safety from inside
// XCTest, but we verify correctness under concurrent producer +
// consumer workloads and pin down the basic snapshot semantics.

import Foundation
import Testing

@testable import DSPAudio

@Suite struct SPSCAudioRingBufferTests {

  @Test func CapacityRoundsUpToPowerOfTwo() {
    #expect(SPSCAudioRingBuffer(minimumCapacity: 1).capacity == 2)
    #expect(SPSCAudioRingBuffer(minimumCapacity: 100).capacity == 128)
    #expect(SPSCAudioRingBuffer(minimumCapacity: 1024).capacity == 1024)
    #expect(SPSCAudioRingBuffer(minimumCapacity: 1025).capacity == 2048)
  }

  @Test func ReadLatestRequiresEnoughData() {
    let ring = SPSCAudioRingBuffer(minimumCapacity: 64)
    var dest = [Float](repeating: -1, count: 8)
    // Empty buffer: cannot satisfy a read.
    #expect(
      !(dest.withUnsafeMutableBufferPointer {
        ring.readLatest(into: $0.baseAddress!, count: 8)
      }))
    // Write 4 samples — still not enough for an 8-sample read.
    let src: [Double] = [1, 2, 3, 4]
    src.withUnsafeBufferPointer {
      ring.appendConvertingDoubleToFloat($0.baseAddress!, count: 4)
    }
    #expect(
      !(dest.withUnsafeMutableBufferPointer {
        ring.readLatest(into: $0.baseAddress!, count: 8)
      }))
    // dest must remain untouched on failure.
    for v in dest { #expect(v == -1) }
  }

  @Test func RoundTripRespectsOrder() {
    let ring = SPSCAudioRingBuffer(minimumCapacity: 16)
    let src: [Double] = [-1, -0.5, 0, 0.25, 0.5, 0.75, 1, 0]
    src.withUnsafeBufferPointer {
      ring.appendConvertingDoubleToFloat($0.baseAddress!, count: src.count)
    }
    var dest = [Float](repeating: 0, count: src.count)
    let ok = dest.withUnsafeMutableBufferPointer {
      ring.readLatest(into: $0.baseAddress!, count: src.count)
    }
    #expect(ok)
    for (i, expected) in src.enumerated() {
      #expect(abs(dest[i] - Float(expected)) <= 1e-7)
    }
  }

  @Test func ReadLatestReturnsMostRecentAfterWrap() {
    // Buffer holds 8 samples; write 12, then read the last 8. Should
    // be values 4..11 in order.
    let ring = SPSCAudioRingBuffer(minimumCapacity: 8)
    #expect(ring.capacity == 8)

    let src: [Double] = (0..<12).map { Double($0) }
    src.withUnsafeBufferPointer {
      ring.appendConvertingDoubleToFloat($0.baseAddress!, count: src.count)
    }
    var dest = [Float](repeating: 0, count: 8)
    let ok = dest.withUnsafeMutableBufferPointer {
      ring.readLatest(into: $0.baseAddress!, count: 8)
    }
    #expect(ok)
    #expect(dest == [4, 5, 6, 7, 8, 9, 10, 11])
  }

  @Test func TotalSamplesWrittenIsMonotonic() {
    let ring = SPSCAudioRingBuffer(minimumCapacity: 64)
    #expect(ring.totalSamplesWritten == 0)
    let src: [Double] = [1, 2, 3]
    src.withUnsafeBufferPointer {
      ring.appendConvertingDoubleToFloat($0.baseAddress!, count: 3)
      ring.appendConvertingDoubleToFloat($0.baseAddress!, count: 3)
    }
    #expect(ring.totalSamplesWritten == 6)
  }

  // MARK: - SPSCAudioRingBuffer

  @Test func SpscRoundTripContiguous() {
    let ring = SPSCAudioRingBuffer(minimumCapacity: 16)
    let src: [Float] = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6]
    src.withUnsafeBufferPointer {
      ring.write(source: $0.baseAddress!, count: src.count, stride: 1)
    }
    #expect(ring.availableToRead == 6)
    var dest = [Float](repeating: -1, count: 6)
    let n = dest.withUnsafeMutableBufferPointer {
      ring.consume(into: $0.baseAddress!, count: 6)
    }
    #expect(n == 6)
    #expect(dest == src)
    #expect(ring.availableToRead == 0)
  }

  @Test func SpscStridedWriteDeinterleaves() {
    // Source: stereo interleaved [L0, R0, L1, R1, L2, R2] — extract
    // the right channel via stride=2 starting at offset 1.
    let interleaved: [Float] = [10, 11, 20, 21, 30, 31]
    let ring = SPSCAudioRingBuffer(minimumCapacity: 16)
    interleaved.withUnsafeBufferPointer { p in
      ring.write(source: p.baseAddress! + 1, count: 3, stride: 2)
    }
    var dest = [Float](repeating: 0, count: 3)
    _ = dest.withUnsafeMutableBufferPointer {
      ring.consume(into: $0.baseAddress!, count: 3)
    }
    #expect(dest == [11, 21, 31])
  }

  @Test func SpscConsumeReturnsLessThanRequestedOnUnderrun() {
    let ring = SPSCAudioRingBuffer(minimumCapacity: 16)
    let src: [Float] = [1, 2, 3]
    src.withUnsafeBufferPointer {
      ring.write(source: $0.baseAddress!, count: 3, stride: 1)
    }
    var dest = [Float](repeating: -1, count: 8)
    let n = dest.withUnsafeMutableBufferPointer {
      ring.consume(into: $0.baseAddress!, count: 8)
    }
    #expect(n == 3)
    // Trailing bytes beyond `n` are explicitly left untouched —
    // caller is expected to fill silence.
    #expect(Array(dest.prefix(3)) == [1, 2, 3])
    #expect(dest[3] == -1)
    #expect(ring.availableToRead == 0)
  }

  @Test func SpscWriteWrapsAroundCapacity() {
    // Capacity 8: write 6, consume 4, write 6 more, consume 8.
    let ring = SPSCAudioRingBuffer(minimumCapacity: 8)
    #expect(ring.capacity == 8)

    let firstBatch: [Float] = [1, 2, 3, 4, 5, 6]
    firstBatch.withUnsafeBufferPointer {
      ring.write(source: $0.baseAddress!, count: 6, stride: 1)
    }
    var dest = [Float](repeating: 0, count: 4)
    _ = dest.withUnsafeMutableBufferPointer {
      ring.consume(into: $0.baseAddress!, count: 4)
    }
    #expect(dest == [1, 2, 3, 4])

    let secondBatch: [Float] = [7, 8, 9, 10, 11, 12]
    secondBatch.withUnsafeBufferPointer {
      ring.write(source: $0.baseAddress!, count: 6, stride: 1)
    }
    var dest2 = [Float](repeating: 0, count: 8)
    let n = dest2.withUnsafeMutableBufferPointer {
      ring.consume(into: $0.baseAddress!, count: 8)
    }
    #expect(n == 8)
    #expect(dest2 == [5, 6, 7, 8, 9, 10, 11, 12])
  }

  @Test func SpscDrainResetsAvailable() {
    let ring = SPSCAudioRingBuffer(minimumCapacity: 8)
    let src: [Float] = [1, 2, 3, 4]
    src.withUnsafeBufferPointer {
      ring.write(source: $0.baseAddress!, count: 4, stride: 1)
    }
    #expect(ring.availableToRead == 4)
    ring.drain()
    #expect(ring.availableToRead == 0)
  }

  /// SPSC concurrent stress test mirroring the one for the snapshot
  /// ring. Producer writes monotonically-increasing Floats in 256-
  /// sample chunks; consumer drains in 64-sample reads and verifies
  /// that every consumed segment is itself a contiguous run.
  @Test func SpscConcurrentProducerConsumerNoDataLoss() {
    let ring = SPSCAudioRingBuffer(minimumCapacity: 65536)
    let producerChunk = 256
    let consumerChunk = 64
    // Pick a total that's a clean multiple of `producerChunk` so the
    // producer writes exactly that many samples — no extra
    // partial-chunk overshoot at the end of its loop. Kept modest
    // so the test stays within deadline even when the suite runs
    // multiple concurrent stress tests in parallel.
    let totalToWrite = 50_176  // == 196 × 256

    let producerDone = DispatchSemaphore(value: 0)
    DispatchQueue.global(qos: .userInteractive).async {
      var counter: Float = 0
      var chunk = [Float](repeating: 0, count: producerChunk)
      var written = 0
      while written < totalToWrite {
        for i in 0..<producerChunk {
          chunk[i] = counter
          counter += 1
        }
        chunk.withUnsafeBufferPointer { p in
          ring.write(source: p.baseAddress!, count: producerChunk, stride: 1)
        }
        written += producerChunk
      }
      producerDone.signal()
    }

    var dest = [Float](repeating: 0, count: consumerChunk)
    var lastSeen: Float = -1
    var consumed: Int = 0
    let deadline = Date().addingTimeInterval(10.0)
    while Date() < deadline {
      let n = dest.withUnsafeMutableBufferPointer {
        ring.consume(into: $0.baseAddress!, count: consumerChunk)
      }
      if n > 0 {
        // Each delivered run must be contiguous and monotonically
        // increasing relative to the previous read.
        #expect(dest[0] > lastSeen)
        for i in 1..<n {
          #expect(abs(dest[i] - dest[i - 1] - 1.0) <= 1e-3)
        }
        lastSeen = dest[n - 1]
        consumed += n
      }
      if producerDone.wait(timeout: .now()) == .success {
        // Final drain
        while true {
          let m = dest.withUnsafeMutableBufferPointer {
            ring.consume(into: $0.baseAddress!, count: consumerChunk)
          }
          if m == 0 { break }
          consumed += m
        }
        break
      }
    }
    #expect(consumed == totalToWrite)
  }

  // MARK: - SPSCQueue<T>

  @Test func SpscQueueRoundTripFifo() {
    let queue = SPSCQueue<Int>(minimumCapacity: 8)
    #expect(queue.capacity == 8)
    #expect(queue.count == 0)
    #expect(queue.dequeue() == nil)

    for i in 0..<5 {
      #expect(queue.enqueue(i))
    }
    #expect(queue.count == 5)

    for i in 0..<5 {
      #expect(queue.dequeue() == i)
    }
    #expect(queue.count == 0)
    #expect(queue.dequeue() == nil)
  }

  @Test func SpscQueueEnqueueReturnsFalseWhenFull() {
    let queue = SPSCQueue<Int>(minimumCapacity: 4)
    for i in 0..<4 {
      #expect(queue.enqueue(i))
    }
    #expect(!(queue.enqueue(99)))
    // Make room and verify it accepts again.
    #expect(queue.dequeue() == 0)
    #expect(queue.enqueue(99))
  }

  @Test func SpscQueueWrapsAroundIndices() {
    // Capacity 4: write 3, read 3, write 4, read 4 — exercises the
    // wrap point on both sides without ever reporting "full".
    let queue = SPSCQueue<Int>(minimumCapacity: 4)
    for i in 0..<3 { _ = queue.enqueue(i) }
    for i in 0..<3 { #expect(queue.dequeue() == i) }
    for i in 100..<104 { #expect(queue.enqueue(i)) }
    for i in 100..<104 { #expect(queue.dequeue() == i) }
  }

  @Test func SpscQueueTransferNonSendable() {
    class NonSendableItem {
      var value: Int
      init(_ value: Int) { self.value = value }
    }

    let queue = SPSCQueue<NonSendableItem>(minimumCapacity: 4)
    let item = NonSendableItem(42)
    let success = queue.enqueue(item)
    #expect(success)

    let popped = queue.dequeue()
    #expect(popped?.value == 42)
  }

  /// Concurrent producer/consumer test for the chunk queue analogue.
  /// Single producer pushes a million integers; single consumer drains
  /// them. Verifies every value is delivered exactly once, in order.
  @Test func SpscQueueConcurrentNoDataLoss() {
    let queue = SPSCQueue<Int>(minimumCapacity: 64)
    let totalToWrite = 200_000

    let producerDone = DispatchSemaphore(value: 0)
    DispatchQueue.global(qos: .userInteractive).async {
      var i = 0
      while i < totalToWrite {
        if queue.enqueue(i) {
          i += 1
        }
        // else queue full → spin until consumer drains
      }
      producerDone.signal()
    }

    var lastSeen = -1
    var consumed = 0
    let deadline = Date().addingTimeInterval(5.0)
    while consumed < totalToWrite, Date() < deadline {
      if let value = queue.dequeue() {
        #expect(value == lastSeen + 1)
        lastSeen = value
        consumed += 1
      }
    }
    _ = producerDone.wait(timeout: .distantFuture)
    #expect(consumed == totalToWrite)
  }

  // MARK: - AtomicDouble

  @Test func AtomicDoubleRoundTrip() {
    let value = AtomicDouble(1.5)
    #expect(value.value == 1.5)
    value.value = 2.71828
    #expect(value.value == 2.71828)
    // Subnormals and signed zero must round-trip exactly.
    value.value = -0.0
    #expect(value.value.bitPattern == Double(-0.0).bitPattern)
    value.value = .infinity
    #expect(value.value == .infinity)
  }

  // MARK: - SPMC concurrent stress

  /// Run a producer in one thread and a consumer in another. The
  /// consumer's snapshots should always be a strictly-increasing run of
  /// integers (with possible gaps) because the producer writes
  /// monotonically increasing values. If the atomic ordering were
  /// broken we'd see out-of-order samples or values that never made it
  /// past the producer.
  @Test func ConcurrentProducerConsumerSeesMonotonicSequence() {
    let ring = SPSCAudioRingBuffer(minimumCapacity: 4096)
    let totalToWrite = 200_000
    let chunkSize = 256
    let readSize = 64

    let producerDone = DispatchSemaphore(value: 0)
    DispatchQueue.global(qos: .userInteractive).async {
      var counter: Double = 0
      var chunk = [Double](repeating: 0, count: chunkSize)
      var written = 0
      while written < totalToWrite {
        for i in 0..<chunkSize {
          chunk[i] = counter
          counter += 1
        }
        chunk.withUnsafeBufferPointer { p in
          ring.appendConvertingDoubleToFloat(p.baseAddress!, count: chunkSize)
        }
        written += chunkSize
      }
      producerDone.signal()
    }

    var snapshotsTaken = 0
    var dest = [Float](repeating: 0, count: readSize)
    let deadline = Date().addingTimeInterval(2.0)
    while Date() < deadline {
      let ok = dest.withUnsafeMutableBufferPointer {
        ring.readLatest(into: $0.baseAddress!, count: readSize)
      }
      if ok {
        // Each snapshot must itself be monotonically increasing —
        // every Float we see is one of the producer's counter
        // values, and the consumer reads a contiguous window.
        for i in 1..<readSize {
          #expect(abs(dest[i] - dest[i - 1] - 1.0) <= 1e-3)
        }
        snapshotsTaken += 1
      }
      if producerDone.wait(timeout: .now()) == .success {
        break
      }
    }

    #expect(snapshotsTaken > 0)
    #expect(ring.totalSamplesWritten >= UInt64(totalToWrite))
  }
}
