import Foundation
import Testing

@testable import DSPAudio

@Suite struct AudioBuffersTests {
  @Test func AllocatesZeroedStorage() {
    let buffers = AudioBuffers(channels: 4, capacity: 32)
    #expect(buffers.channels == 4)
    #expect(buffers.capacity == 32)
    for ch in 0..<4 {
      let buf = buffers[ch]
      #expect(buf.count == 32)
      for i in 0..<32 {
        #expect(buf[i] == 0.0)
      }
    }
  }

  @Test func ChannelPointersAreStable() {
    let buffers = AudioBuffers(channels: 2, capacity: 16)
    let p0 = buffers[0].baseAddress!
    let p1 = buffers[1].baseAddress!
    // Layout is contiguous: channel 1 starts exactly `capacity` samples after channel 0.
    #expect(p1 == p0 + 16)
    // Pointer is the same on every fetch.
    #expect(buffers[0].baseAddress == p0)
    #expect(buffers[1].baseAddress == p1)
  }

  @Test func WritesAreIsolatedPerChannel() {
    let buffers = AudioBuffers(channels: 3, capacity: 8)
    for ch in 0..<3 {
      let buf = buffers[ch]
      for i in 0..<8 {
        buf[i] = Double(ch * 100 + i)
      }
    }
    for ch in 0..<3 {
      let buf = buffers[ch]
      for i in 0..<8 {
        #expect(buf[i] == Double(ch * 100 + i))
      }
    }
  }

  @Test func CopyingInitMatchesSource() {
    let waveforms: [[Double]] = [
      [1.0, 2.0, 3.0, 4.0],
      [-1.0, -2.0, -3.0, -4.0],
    ]
    let buffers = AudioBuffers(copying: waveforms)
    #expect(buffers.channels == 2)
    #expect(buffers.capacity >= 4)
    for ch in 0..<2 {
      let snapshot = buffers.snapshotChannel(ch, count: 4)
      #expect(snapshot == waveforms[ch])
    }
  }

  @Test func CopyingInitZeroPadsShorterChannels() {
    let waveforms: [[Double]] = [
      [1.0, 2.0, 3.0, 4.0],
      [9.0, 8.0],  // shorter
    ]
    let buffers = AudioBuffers(copying: waveforms)
    #expect(buffers.capacity == 4)
    #expect(buffers.snapshotChannel(0) == [1.0, 2.0, 3.0, 4.0])
    #expect(buffers.snapshotChannel(1) == [9.0, 8.0, 0.0, 0.0])
  }

  @Test func MutationThroughCachedPointer() {
    let buffers = AudioBuffers(channels: 2, capacity: 4)
    // Caching the pointer once should still see updates done through the
    // subscript — they're aliases of the same storage.
    let cached = buffers[0]
    buffers[0][2] = 42.0
    #expect(cached[2] == 42.0)
    cached[3] = 99.0
    #expect(buffers[0][3] == 99.0)
  }
}
