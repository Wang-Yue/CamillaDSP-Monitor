import XCTest

@testable import CamillaDSPLib

final class AudioHistoryBufferTests: XCTestCase {

  func testReset() {
    let buffer = AudioHistoryBuffer()
    XCTAssertEqual(buffer.channels, 0)
    XCTAssertFalse(buffer.hasData)

    buffer.reset(channels: 2)
    XCTAssertEqual(buffer.channels, 2)
    XCTAssertFalse(buffer.hasData)
  }

  func testAppendAndRead() throws {
    let buffer = AudioHistoryBuffer()
    buffer.reset(channels: 2)

    var chunk = AudioChunk(frames: 1024, channels: 2)
    for t in 0..<1024 {
      chunk.waveforms[0][t] = Double(t)
      chunk.waveforms[1][t] = Double(t * 2)
    }

    buffer.append(chunk: chunk)
    XCTAssertTrue(buffer.hasData)

    let dest = UnsafeMutablePointer<Float>.allocate(capacity: 1024)
    defer { dest.deallocate() }

    // Read channel 0
    let ok0 = try buffer.readLatest(into: dest, count: 1024, channel: 0)
    XCTAssertTrue(ok0)
    XCTAssertEqual(dest[0], 0.0)
    XCTAssertEqual(dest[1023], 1023.0)

    // Read channel 1
    let ok1 = try buffer.readLatest(into: dest, count: 1024, channel: 1)
    XCTAssertTrue(ok1)
    XCTAssertEqual(dest[0], 0.0)
    XCTAssertEqual(dest[1023], 2046.0)
  }

  func testReadLatestAverageChannels() throws {
    let buffer = AudioHistoryBuffer()
    buffer.reset(channels: 2)

    var chunk = AudioChunk(frames: 1024, channels: 2)
    for t in 0..<1024 {
      chunk.waveforms[0][t] = 1.0
      chunk.waveforms[1][t] = 3.0
    }

    buffer.append(chunk: chunk)

    let dest = UnsafeMutablePointer<Float>.allocate(capacity: 1024)
    defer { dest.deallocate() }

    // Read average (channel = nil)
    let ok = try buffer.readLatest(into: dest, count: 1024, channel: nil)
    XCTAssertTrue(ok)
    // Average of 1.0 and 3.0 is 2.0!
    XCTAssertEqual(dest[0], 2.0, accuracy: 1e-5)
    XCTAssertEqual(dest[1023], 2.0, accuracy: 1e-5)
  }

  func testReadLatestEmpty() {
    let buffer = AudioHistoryBuffer()
    let dest = UnsafeMutablePointer<Float>.allocate(capacity: 1024)
    defer { dest.deallocate() }

    // Buffer not initialized (channels = 0)
    XCTAssertThrowsError(try buffer.readLatest(into: dest, count: 1024, channel: nil)) { error in
      guard case SpectrumError.bufferEmpty = error else {
        return XCTFail("Expected bufferEmpty, got \(error)")
      }
    }
  }

  func testReadLatestChannelOutOfRange() {
    let buffer = AudioHistoryBuffer()
    buffer.reset(channels: 2)
    let dest = UnsafeMutablePointer<Float>.allocate(capacity: 1024)
    defer { dest.deallocate() }

    XCTAssertThrowsError(try buffer.readLatest(into: dest, count: 1024, channel: 2)) { error in
      guard case SpectrumError.channelOutOfRange(let ch, let avail) = error else {
        return XCTFail("Expected channelOutOfRange, got \(error)")
      }
      XCTAssertEqual(ch, 2)
      XCTAssertEqual(avail, 2)
    }
  }

  func testAppendMismatchedChannels() {
    let buffer = AudioHistoryBuffer()
    buffer.reset(channels: 2)

    let chunk = AudioChunk(frames: 1024, channels: 1)  // Mismatch!
    buffer.append(chunk: chunk)
    XCTAssertFalse(buffer.hasData)  // Should ignore
  }
}
