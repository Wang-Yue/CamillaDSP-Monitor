import DSPConfig
import Foundation

enum ConvCoefficientLoader {
  static func loadWAV(path: String, channel: Int) throws -> [Double] {
    let url = URL(fileURLWithPath: path)
    guard FileManager.default.fileExists(atPath: path) else {
      throw ConfigError.invalidFilter("WAV file not found: \(path)")
    }
    let data = try Data(contentsOf: url)
    guard data.count > 44 else {
      throw ConfigError.invalidFilter("WAV file too small: \(path)")
    }

    let numChannels = data.withUnsafeBytes { $0.load(fromByteOffset: 22, as: UInt16.self) }
    let bitsPerSample = data.withUnsafeBytes { $0.load(fromByteOffset: 34, as: UInt16.self) }
    let dataSize = data.withUnsafeBytes { $0.load(fromByteOffset: 40, as: UInt32.self) }

    guard channel < Int(numChannels) else {
      throw ConfigError.invalidFilter(
        "WAV channel \(channel) out of range (file has \(numChannels) channels)")
    }

    let bytesPerSample = Int(bitsPerSample) / 8
    let numFrames = Int(dataSize) / (Int(numChannels) * bytesPerSample)
    var result = [Double](repeating: 0, count: numFrames)
    let headerSize = 44

    for frame in 0..<numFrames {
      let offset = headerSize + (frame * Int(numChannels) + channel) * bytesPerSample
      guard offset + bytesPerSample <= data.count else { break }
      switch bitsPerSample {
      case 16:
        let raw = data.withUnsafeBytes { $0.load(fromByteOffset: offset, as: Int16.self) }
        result[frame] = Double(raw) / Double(Int16.max)
      case 24:
        let b0 = Int32(data[offset])
        let b1 = Int32(data[offset + 1])
        let b2 = Int32(data[offset + 2])
        var raw = b0 | (b1 << 8) | (b2 << 16)
        if raw & 0x800000 != 0 { raw |= -0x800000 }
        result[frame] = Double(raw) / Double((1 << 23) - 1)
      case 32:
        let raw = data.withUnsafeBytes { $0.load(fromByteOffset: offset, as: Float.self) }
        result[frame] = Double(raw)
      case 64:
        let raw = data.withUnsafeBytes { $0.load(fromByteOffset: offset, as: Double.self) }
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
