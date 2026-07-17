import Foundation
import Testing

@testable import DSPMonitor
@testable import DSPConfig

@Suite struct DeviceConfigTests {
  
  private func writeDummyWav(channels: UInt16, sampleRate: UInt32) -> String {
    let tmpDir = FileManager.default.temporaryDirectory
    let fileURL = tmpDir.appendingPathComponent("test_\(UUID().uuidString).wav")
    
    var data = Data()
    
    // RIFF header
    data.append("RIFF".data(using: .utf8)!)
    var fileSize: UInt32 = 36 // 44 - 8
    data.append(Data(bytes: &fileSize, count: 4))
    data.append("WAVE".data(using: .utf8)!)
    
    // fmt chunk
    data.append("fmt ".data(using: .utf8)!)
    var subchunk1Size: UInt32 = 16
    data.append(Data(bytes: &subchunk1Size, count: 4))
    var audioFormat: UInt16 = 1 // PCM
    data.append(Data(bytes: &audioFormat, count: 2))
    
    var numChannels = channels
    data.append(Data(bytes: &numChannels, count: 2))
    
    var rate = sampleRate
    data.append(Data(bytes: &rate, count: 4))
    
    var byteRate: UInt32 = sampleRate * UInt32(channels) * 2
    data.append(Data(bytes: &byteRate, count: 4))
    
    var blockAlign: UInt16 = channels * 2
    data.append(Data(bytes: &blockAlign, count: 2))
    
    var bitsPerSample: UInt16 = 16
    data.append(Data(bytes: &bitsPerSample, count: 2))
    
    // data chunk
    data.append("data".data(using: .utf8)!)
    var subchunk2Size: UInt32 = 0
    data.append(Data(bytes: &subchunk2Size, count: 4))
    
    try! data.write(to: fileURL)
    return fileURL.path
  }
  
  @Test func testParseWavHeader() throws {
    let path = writeDummyWav(channels: 6, sampleRate: 96000)
    defer {
      try? FileManager.default.removeItem(atPath: path)
    }
    
    let info = DeviceConfig.parseWavHeader(atPath: path)
    #expect(info != nil)
    #expect(info?.channels == 6)
    #expect(info?.sampleRate == 96000)
  }
  
  @Test func testEnforcedWithWavFile() throws {
    let path = writeDummyWav(channels: 8, sampleRate: 44100)
    defer {
      try? FileManager.default.removeItem(atPath: path)
    }
    
    var config = DeviceConfig()
    config.backend = .wavFile
    config.filename = path
    config.channels = 2 // start with something else
    config.sampleRate = 48000
    
    let enforced = config.enforced()
    #expect(enforced.channels == 8)
    #expect(enforced.sampleRate == 44100)
  }

  private func writeDummyRF64(channels: UInt16, sampleRate: UInt32) -> String {
    let tmpDir = FileManager.default.temporaryDirectory
    let fileURL = tmpDir.appendingPathComponent("test_rf64_\(UUID().uuidString).wav")
    
    var data = Data()
    
    // RF64 header
    data.append("RF64".data(using: .utf8)!)
    var fileSize: UInt32 = 0xFFFFFFFF
    data.append(Data(bytes: &fileSize, count: 4))
    data.append("WAVE".data(using: .utf8)!)
    
    // ds64 chunk
    data.append("ds64".data(using: .utf8)!)
    var ds64Size: UInt32 = 28
    data.append(Data(bytes: &ds64Size, count: 4))
    var riffSize: UInt64 = 1000
    data.append(Data(bytes: &riffSize, count: 8))
    var dataSize: UInt64 = 500
    data.append(Data(bytes: &dataSize, count: 8))
    var sampleCount: UInt64 = 250
    data.append(Data(bytes: &sampleCount, count: 8))
    var tableEntryCount: UInt32 = 0
    data.append(Data(bytes: &tableEntryCount, count: 4))

    // fmt chunk
    data.append("fmt ".data(using: .utf8)!)
    var subchunk1Size: UInt32 = 16
    data.append(Data(bytes: &subchunk1Size, count: 4))
    var audioFormat: UInt16 = 1 // PCM
    data.append(Data(bytes: &audioFormat, count: 2))
    
    var numChannels = channels
    data.append(Data(bytes: &numChannels, count: 2))
    
    var rate = sampleRate
    data.append(Data(bytes: &rate, count: 4))
    
    var byteRate: UInt32 = sampleRate * UInt32(channels) * 2
    data.append(Data(bytes: &byteRate, count: 4))
    
    var blockAlign: UInt16 = channels * 2
    data.append(Data(bytes: &blockAlign, count: 2))
    
    var bitsPerSample: UInt16 = 16
    data.append(Data(bytes: &bitsPerSample, count: 2))
    
    // data chunk
    data.append("data".data(using: .utf8)!)
    var subchunk2Size: UInt32 = 0xFFFFFFFF
    data.append(Data(bytes: &subchunk2Size, count: 4))
    
    try! data.write(to: fileURL)
    return fileURL.path
  }
  
  @Test func testParseRF64Header() throws {
    let path = writeDummyRF64(channels: 8, sampleRate: 192000)
    defer {
      try? FileManager.default.removeItem(atPath: path)
    }
    
    let info = DeviceConfig.parseWavHeader(atPath: path)
    #expect(info != nil)
    #expect(info?.channels == 8)
    #expect(info?.sampleRate == 192000)
  }
}
