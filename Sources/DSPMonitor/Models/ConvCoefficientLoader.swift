import DSPConfig
import Foundation

enum ConvCoefficientLoader {
  static func loadWAV(path: String, channel: Int) throws -> [Double] {
    let url = URL(fileURLWithPath: path)
    guard FileManager.default.fileExists(atPath: path) else {
      throw ConfigError.invalidFilter("WAV file not found: \(path)")
    }
    let data = try Data(contentsOf: url)
    guard data.count > 12 else {
      throw ConfigError.invalidFilter("WAV file too small: \(path)")
    }

    // Check RIFF & WAVE headers
    let riffHeader = String(bytes: data.prefix(4), encoding: .ascii)
    let waveHeader = String(bytes: data[8..<12], encoding: .ascii)
    guard riffHeader == "RIFF", waveHeader == "WAVE" else {
      throw ConfigError.invalidFilter("Invalid WAV format: \(path)")
    }

    var offset = 12
    var audioFormat: UInt16 = 1
    var numChannels: UInt16 = 0
    var bitsPerSample: UInt16 = 0
    var audioDataOffset: Int? = nil
    var audioDataSize: Int = 0

    while offset + 8 <= data.count {
      let chunkID = String(bytes: data[offset..<(offset + 4)], encoding: .ascii) ?? ""
      let chunkSize = Int(data.withUnsafeBytes { $0.load(fromByteOffset: offset + 4, as: UInt32.self) })
      let nextOffset = offset + 8 + chunkSize

      if chunkID == "fmt " && chunkSize >= 16 {
        audioFormat = data.withUnsafeBytes { $0.load(fromByteOffset: offset + 8, as: UInt16.self) }
        numChannels = data.withUnsafeBytes { $0.load(fromByteOffset: offset + 10, as: UInt16.self) }
        bitsPerSample = data.withUnsafeBytes { $0.load(fromByteOffset: offset + 22, as: UInt16.self) }
      } else if chunkID == "data" {
        audioDataOffset = offset + 8
        audioDataSize = chunkSize
        break
      }
      offset = nextOffset + (chunkSize % 2) // Word alignment
    }

    guard let dataOffset = audioDataOffset, numChannels > 0, bitsPerSample > 0 else {
      throw ConfigError.invalidFilter("Could not locate audio data chunk in WAV file: \(path)")
    }

    guard channel < Int(numChannels) else {
      throw ConfigError.invalidFilter(
        "WAV channel \(channel) out of range (file has \(numChannels) channels)")
    }

    let bytesPerSample = Int(bitsPerSample) / 8
    guard bytesPerSample > 0 else {
      throw ConfigError.invalidFilter("Invalid bit depth in WAV file: \(bitsPerSample)")
    }
    let numFrames = audioDataSize / (Int(numChannels) * bytesPerSample)
    var result = [Double](repeating: 0, count: numFrames)

    for frame in 0..<numFrames {
      let sampleOffset = dataOffset + (frame * Int(numChannels) + channel) * bytesPerSample
      guard sampleOffset + bytesPerSample <= data.count else { break }
      switch bitsPerSample {
      case 16:
        let raw = data.withUnsafeBytes { $0.load(fromByteOffset: sampleOffset, as: Int16.self) }
        result[frame] = Double(raw) / Double(Int16.max)
      case 24:
        let b0 = Int32(data[sampleOffset])
        let b1 = Int32(data[sampleOffset + 1])
        let b2 = Int32(data[sampleOffset + 2])
        var raw = b0 | (b1 << 8) | (b2 << 16)
        if raw & 0x800000 != 0 { raw |= -0x800000 }
        result[frame] = Double(raw) / Double((1 << 23) - 1)
      case 32:
        if audioFormat == 3 {
          let raw = data.withUnsafeBytes { $0.load(fromByteOffset: sampleOffset, as: Float.self) }
          result[frame] = Double(raw)
        } else {
          let raw = data.withUnsafeBytes { $0.load(fromByteOffset: sampleOffset, as: Int32.self) }
          result[frame] = Double(raw) / Double(Int32.max)
        }
      case 64:
        let raw = data.withUnsafeBytes { $0.load(fromByteOffset: sampleOffset, as: Double.self) }
        result[frame] = Double(raw)
      default:
        throw ConfigError.invalidFilter("Unsupported WAV bit depth: \(bitsPerSample)")
      }
    }
    return result
  }

  static func loadRaw(
    path: String,
    format: String,
    skipBytesLines: Int = 0,
    readBytesLines: Int = 0
  ) throws -> [Double] {
    let url = URL(fileURLWithPath: path)
    guard FileManager.default.fileExists(atPath: path) else {
      throw ConfigError.invalidFilter("Raw file not found: \(path)")
    }

    if format == "TEXT" {
      let text = try String(contentsOf: url, encoding: .utf8)
      var lines = text.split(separator: "\n")
      if skipBytesLines > 0 {
        guard skipBytesLines < lines.count else { return [] }
        lines.removeFirst(skipBytesLines)
      }
      if readBytesLines > 0 {
        lines = Array(lines.prefix(readBytesLines))
      }
      return lines.compactMap {
        Double($0.trimmingCharacters(in: .whitespaces))
      }
    }

    var data = try Data(contentsOf: url)
    if skipBytesLines > 0 {
      guard skipBytesLines < data.count else { return [] }
      data = data.advanced(by: skipBytesLines)
    }
    if readBytesLines > 0 {
      data = data.prefix(readBytesLines)
    }

    switch format {
    case "FLOAT64", "F64_LE":
      let count = data.count / 8
      return data.withUnsafeBytes { buf in
        (0..<count).map { Double(buf.load(fromByteOffset: $0 * 8, as: Double.self)) }
      }
    case "FLOAT32", "F32_LE":
      let count = data.count / 4
      return data.withUnsafeBytes { buf in
        (0..<count).map { Double(buf.load(fromByteOffset: $0 * 4, as: Float.self)) }
      }
    case "S32_LE":
      let count = data.count / 4
      let scale = 1.0 / Double(Int32.max)
      return data.withUnsafeBytes { buf in
        (0..<count).map { Double(buf.load(fromByteOffset: $0 * 4, as: Int32.self)) * scale }
      }
    case "S16_LE":
      let count = data.count / 2
      let scale = 1.0 / Double(Int16.max)
      return data.withUnsafeBytes { buf in
        (0..<count).map { Double(buf.load(fromByteOffset: $0 * 2, as: Int16.self)) * scale }
      }
    default:
      throw ConfigError.invalidFilter("Unsupported raw format: \(format)")
    }
  }
}
