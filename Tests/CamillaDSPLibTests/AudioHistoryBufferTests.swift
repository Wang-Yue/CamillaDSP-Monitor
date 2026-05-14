import Foundation
import Testing

@testable import DSPAudio

@Suite struct AudioHistoryBufferTests {

  @Test func Reset() {
    let buffer = AudioHistoryBuffer()
    #expect(buffer.channels == 0)
    #expect(!(buffer.hasData))

    buffer.reset(channels: 2)
    #expect(buffer.channels == 2)
    #expect(!(buffer.hasData))
  }

  @Test func AppendAndRead() throws {
    let buffer = AudioHistoryBuffer()
    buffer.reset(channels: 2)

    let chunk = AudioChunk(frames: 1024, channels: 2)
    for t in 0..<1024 {
      chunk[0][t] = Double(t)
      chunk[1][t] = Double(t * 2)
    }

    buffer.append(chunk: chunk)
    #expect(buffer.hasData)

    let dest = UnsafeMutablePointer<Float>.allocate(capacity: 1024)
    defer { dest.deallocate() }

    // Read channel 0
    let ok0 = try buffer.readLatest(into: dest, count: 1024, channel: 0)
    #expect(ok0)
    #expect(dest[0] == 0.0)
    #expect(dest[1023] == 1023.0)

    // Read channel 1
    let ok1 = try buffer.readLatest(into: dest, count: 1024, channel: 1)
    #expect(ok1)
    #expect(dest[0] == 0.0)
    #expect(dest[1023] == 2046.0)
  }

  @Test func ReadLatestAverageChannels() throws {
    let buffer = AudioHistoryBuffer()
    buffer.reset(channels: 2)

    let chunk = AudioChunk(frames: 1024, channels: 2)
    for t in 0..<1024 {
      chunk[0][t] = 1.0
      chunk[1][t] = 3.0
    }

    buffer.append(chunk: chunk)

    let dest = UnsafeMutablePointer<Float>.allocate(capacity: 1024)
    defer { dest.deallocate() }

    // Read average (channel = nil)
    let ok = try buffer.readLatest(into: dest, count: 1024, channel: nil)
    #expect(ok)
    // Average of 1.0 and 3.0 is 2.0!
    #expect(abs(dest[0] - 2.0) <= 1e-5)
    #expect(abs(dest[1023] - 2.0) <= 1e-5)
  }

  @Test func ReadLatestEmpty() {
    let buffer = AudioHistoryBuffer()
    let dest = UnsafeMutablePointer<Float>.allocate(capacity: 1024)
    defer { dest.deallocate() }

    // Buffer not initialized (channels = 0)
    do {
      _ = try buffer.readLatest(into: dest, count: 1024, channel: nil)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case AudioHistoryBufferError.bufferEmpty = error else {
        Issue.record("Expected bufferEmpty, got \(error)")
        return
      }

    }
  }

  @Test func ReadLatestChannelOutOfRange() {
    let buffer = AudioHistoryBuffer()
    buffer.reset(channels: 2)
    let dest = UnsafeMutablePointer<Float>.allocate(capacity: 1024)
    defer { dest.deallocate() }

    do {
      _ = try buffer.readLatest(into: dest, count: 1024, channel: 2)
      Issue.record("Expected error to be thrown")
    } catch {
      guard case AudioHistoryBufferError.channelOutOfRange(let ch, let avail) = error else {
        Issue.record("Expected channelOutOfRange, got \(error)")
        return
      }
      #expect(ch == 2)
      #expect(avail == 2)

    }
  }

  @Test func AppendMismatchedChannels() {
    let buffer = AudioHistoryBuffer()
    buffer.reset(channels: 2)

    let chunk = AudioChunk(frames: 1024, channels: 1)  // Mismatch!
    buffer.append(chunk: chunk)
    #expect(!(buffer.hasData))  // Should ignore
  }
}
