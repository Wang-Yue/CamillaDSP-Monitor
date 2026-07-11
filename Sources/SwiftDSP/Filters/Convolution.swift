// Uniform-partitioned overlap-save FIR convolution.
// Stockham-style segmented overlap-save with one 2N-point real FFT per
// chunk and an N+1-bin spectrum-domain multiply-accumulate across the
// segment history.
//
//   - Uses `RealFFT`, which stores the same N+1 unique bins as separate
//     `specRe`/`specIm` arrays. The flat layout (DC at index 0, Nyquist
//     at index N, both with `im == 0`) lets us run the spectrum
//     multiply through `vDSP_zvmulD` / `vDSP_zvmaD` without any DC/
//     Nyquist special-casing.
//   - `RealFFT.inverse` produces `length · signal`. The inverse does not
//     scale, so we pre-divide coefficients by
//     `2 * data_length` to compensate.
//   - All hot-path buffers are owned by raw `UnsafeMutablePointer`s
//     (`AudioBuffers`-style) so `process(waveform:)` cannot trip
//     Swift's Array CoW path that a `[Double]` field would.

import Accelerate
import DSPConfig
import Foundation

/// Source format for the impulse response. Parameters:
///
///   - `.values`: inline IR samples in `values`.
///   - `.wav`:    `filename` (24/16/32f/64f WAV), single channel `channel`.
///   - `.raw`:    `filename` of a flat sample stream, one of FLOAT64,
///                FLOAT32, S32_LE, S16_LE, or TEXT (newline-separated).
///   - `.dummy`:  generates a Kronecker delta of length `length`. Used
///                for sanity-checks; the filter becomes a pure delay.
extension ConvParameters {

  /// Resolve the parameters to a flat IR buffer. Only called from the
  /// control plane (filter creation / hot-swap), never from
  /// `process(waveform:)`.
  public func loadCoefficients(sampleRate: Int) throws -> [Double] {
    switch type {
    case .values:
      return values ?? []
    case .dummy:
      var v = [Double](repeating: 0, count: length ?? 0)
      if !v.isEmpty { v[0] = 1.0 }
      return v
    case .wav:
      guard let f = filename else {
        throw ConfigError.invalidFilter("Conv 'wav' missing filename")
      }
      let resolved = f.replacingOccurrences(of: "$samplerate$", with: "\(sampleRate)")
      return try ConvCoefficientLoader.loadWAV(path: resolved, channel: channel ?? 0)
    case .raw:
      guard let f = filename else {
        throw ConfigError.invalidFilter("Conv 'raw' missing filename")
      }
      let resolved = f.replacingOccurrences(of: "$samplerate$", with: "\(sampleRate)")
      return try ConvCoefficientLoader.loadRaw(
        path: resolved,
        format: format ?? "FLOAT64",
        skipBytesLines: skipBytesLines ?? 0,
        readBytesLines: readBytesLines ?? 0
      )
    }
  }
}

/// Coefficient file readers. Off the audio thread — straightforward
/// `Data`-based parsers, no streaming or memory-mapping.
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

final class ConvolutionFilter: Filter {
  let name: String

  /// Block length `N` (one input chunk per `process` call).
  private let chunkSize: Int
  /// FFT length `2N`.
  private let fftSize: Int
  /// Unique-bin count `N + 1`.
  private let bins: Int

  private let fft: RealFFT

  /// Number of `chunkSize`-long IR segments.
  private var nsegments: Int
  /// Index of the input-history slot most recently filled (mod `nsegments`).
  private var index: Int = 0

  // Time-domain scratch buffers, both `2N` long.
  private let inputBuf: UnsafeMutablePointer<Double>
  private let outputBuf: UnsafeMutablePointer<Double>

  /// Overlap-save state, length `N` — the second half of the previous
  /// IFFT result, summed into the next block's first half.
  private let overlap: UnsafeMutablePointer<Double>

  // Pre-FFT'd IR segments and rolling input-spectrum history. Each is a
  // flat `nsegments * bins` block of `Double`; the per-segment slice for
  // segment `s` lives at `[s * bins ..< (s + 1) * bins]`.
  private var coeffsFRe: UnsafeMutablePointer<Double>
  private var coeffsFIm: UnsafeMutablePointer<Double>
  private var inputFRe: UnsafeMutablePointer<Double>
  private var inputFIm: UnsafeMutablePointer<Double>

  /// Per-call accumulator for `Σ_seg input_F[hist] · coeffs_F[seg]`.
  private let tempRe: UnsafeMutablePointer<Double>
  private let tempIm: UnsafeMutablePointer<Double>

  /// Build a convolution filter from raw IR samples.
  ///
  /// - Parameters:
  ///   - coefficients: Impulse response, in time-domain sample order.
  ///     Must be non-empty.
  ///   - chunkSize: Per-call block length `N`. Must match the
  ///     `validFrames` the pipeline will hand to `process`.
  init(name: String = "convolution", coefficients: [Double], chunkSize: Int) throws {
    guard chunkSize > 0 else {
      throw ConfigError.invalidFilter("ConvolutionFilter: chunkSize must be > 0, got \(chunkSize)")
    }
    guard !coefficients.isEmpty else {
      throw ConfigError.invalidFilter("ConvolutionFilter: coefficients must not be empty")
    }
    self.name = name
    self.chunkSize = chunkSize
    self.fftSize = 2 * chunkSize
    self.bins = chunkSize + 1
    self.fft = try RealFFT(length: 2 * chunkSize)

    let ns = (coefficients.count + chunkSize - 1) / chunkSize
    self.nsegments = ns

    self.inputBuf = .allocate(capacity: 2 * chunkSize)
    self.outputBuf = .allocate(capacity: 2 * chunkSize)
    self.overlap = .allocate(capacity: chunkSize)
    self.coeffsFRe = .allocate(capacity: ns * (chunkSize + 1))
    self.coeffsFIm = .allocate(capacity: ns * (chunkSize + 1))
    self.inputFRe = .allocate(capacity: ns * (chunkSize + 1))
    self.inputFIm = .allocate(capacity: ns * (chunkSize + 1))
    self.tempRe = .allocate(capacity: chunkSize + 1)
    self.tempIm = .allocate(capacity: chunkSize + 1)

    self.inputBuf.initialize(repeating: 0, count: 2 * chunkSize)
    self.outputBuf.initialize(repeating: 0, count: 2 * chunkSize)
    self.overlap.initialize(repeating: 0, count: chunkSize)
    self.coeffsFRe.initialize(repeating: 0, count: ns * (chunkSize + 1))
    self.coeffsFIm.initialize(repeating: 0, count: ns * (chunkSize + 1))
    self.inputFRe.initialize(repeating: 0, count: ns * (chunkSize + 1))
    self.inputFIm.initialize(repeating: 0, count: ns * (chunkSize + 1))
    self.tempRe.initialize(repeating: 0, count: chunkSize + 1)
    self.tempIm.initialize(repeating: 0, count: chunkSize + 1)

    Self.fftCoefficients(
      coefficients,
      chunkSize: chunkSize,
      nsegments: ns,
      fft: self.fft,
      coeffsFRe: self.coeffsFRe,
      coeffsFIm: self.coeffsFIm)
  }

  /// Convenience initialiser that resolves `ConvParameters` to a flat
  /// IR buffer first (control plane only, may touch the filesystem).
  convenience init(
    name: String = "convolution",
    parameters: ConvParameters,
    chunkSize: Int,
    sampleRate: Int
  ) throws {
    try parameters.validate()
    let coeffs = try parameters.loadCoefficients(sampleRate: sampleRate)
    guard !coeffs.isEmpty else {
      throw ConfigError.invalidFilter("Conv filter resolved to empty IR")
    }
    try self.init(name: name, coefficients: coeffs, chunkSize: chunkSize)
  }

  deinit {
    inputBuf.deinitialize(count: fftSize)
    outputBuf.deinitialize(count: fftSize)
    overlap.deinitialize(count: chunkSize)
    let histCount = nsegments * bins
    coeffsFRe.deinitialize(count: histCount)
    coeffsFIm.deinitialize(count: histCount)
    inputFRe.deinitialize(count: histCount)
    inputFIm.deinitialize(count: histCount)
    tempRe.deinitialize(count: bins)
    tempIm.deinitialize(count: bins)

    inputBuf.deallocate()
    outputBuf.deallocate()
    overlap.deallocate()
    coeffsFRe.deallocate()
    coeffsFIm.deallocate()
    inputFRe.deallocate()
    inputFIm.deallocate()
    tempRe.deallocate()
    tempIm.deallocate()
  }

  /// Process one block in-place. The hot path is allocation-free in
  /// steady state; everything below is pointer arithmetic over the
  /// preallocated storage from `init`.
  func process(waveform: MutableWaveform) {
    let count = min(waveform.count, chunkSize)
    guard let wBase = waveform.baseAddress else { return }

    // 1. Stage the new block in the first `chunkSize` samples of
    //    `inputBuf`; zero the second half (the FFT zero-pad) and any
    //    short tail of the first half (when `count < chunkSize`).
    inputBuf.update(from: wBase, count: count)
    if count < chunkSize {
      vDSP_vclrD(inputBuf + count, 1, vDSP_Length(chunkSize - count))
    }
    vDSP_vclrD(inputBuf + chunkSize, 1, vDSP_Length(chunkSize))

    // 2. Advance the history index and FFT the new block into that
    //    slot. The slot now holds the spectrum of `inputBuf`.
    index = (index + 1) % nsegments
    let inSlotRe = inputFRe + index * bins
    let inSlotIm = inputFIm + index * bins
    fft.forward(realIn: inputBuf, specRe: inSlotRe, specIm: inSlotIm)

    // 3. Spectrum-domain multiply-accumulate across the segment
    //    history. seg=0 pairs the newest input with coeff[0]; seg=k
    //    pairs the input from `k` blocks ago with coeff[k].
    vDSP_vclrD(tempRe, 1, vDSP_Length(bins))
    vDSP_vclrD(tempIm, 1, vDSP_Length(bins))

    for seg in 0..<nsegments {
      let histIdx = (index + nsegments - seg) % nsegments
      let inRe = inputFRe + histIdx * bins
      let inIm = inputFIm + histIdx * bins
      let coRe = coeffsFRe + seg * bins
      let coIm = coeffsFIm + seg * bins

      let vecLen = (bins / 8) * 8

      let p_hre = UnsafeRawPointer(inRe).assumingMemoryBound(to: SIMD2<Double>.self)
      let p_him = UnsafeRawPointer(inIm).assumingMemoryBound(to: SIMD2<Double>.self)
      let p_sre = UnsafeRawPointer(coRe).assumingMemoryBound(to: SIMD2<Double>.self)
      let p_sim = UnsafeRawPointer(coIm).assumingMemoryBound(to: SIMD2<Double>.self)
      let p_acc_re = UnsafeMutableRawPointer(tempRe).assumingMemoryBound(to: SIMD2<Double>.self)
      let p_acc_im = UnsafeMutableRawPointer(tempIm).assumingMemoryBound(to: SIMD2<Double>.self)

      for k in stride(from: 0, to: vecLen / 2, by: 4) {
        let h_re0 = p_hre[k]
        let h_im0 = p_him[k]
        let s_re0 = p_sre[k]
        let s_im0 = p_sim[k]
        var a_re0 = p_acc_re[k]
        var a_im0 = p_acc_im[k]

        let h_re1 = p_hre[k + 1]
        let h_im1 = p_him[k + 1]
        let s_re1 = p_sre[k + 1]
        let s_im1 = p_sim[k + 1]
        var a_re1 = p_acc_re[k + 1]
        var a_im1 = p_acc_im[k + 1]

        let h_re2 = p_hre[k + 2]
        let h_im2 = p_him[k + 2]
        let s_re2 = p_sre[k + 2]
        let s_im2 = p_sim[k + 2]
        var a_re2 = p_acc_re[k + 2]
        var a_im2 = p_acc_im[k + 2]

        let h_re3 = p_hre[k + 3]
        let h_im3 = p_him[k + 3]
        let s_re3 = p_sre[k + 3]
        let s_im3 = p_sim[k + 3]
        var a_re3 = p_acc_re[k + 3]
        var a_im3 = p_acc_im[k + 3]

        a_re0 += h_re0 * s_re0 - h_im0 * s_im0
        a_im0 += h_re0 * s_im0 + h_im0 * s_re0

        a_re1 += h_re1 * s_re1 - h_im1 * s_im1
        a_im1 += h_re1 * s_im1 + h_im1 * s_re1

        a_re2 += h_re2 * s_re2 - h_im2 * s_im2
        a_im2 += h_re2 * s_im2 + h_im2 * s_re2

        a_re3 += h_re3 * s_re3 - h_im3 * s_im3
        a_im3 += h_re3 * s_im3 + h_im3 * s_re3

        p_acc_re[k] = a_re0
        p_acc_im[k] = a_im0
        p_acc_re[k + 1] = a_re1
        p_acc_im[k + 1] = a_im1
        p_acc_re[k + 2] = a_re2
        p_acc_im[k + 2] = a_im2
        p_acc_re[k + 3] = a_re3
        p_acc_im[k + 3] = a_im3
      }

      for k in vecLen..<bins {
        tempRe[k] += inRe[k] * coRe[k] - inIm[k] * coIm[k]
        tempIm[k] += inRe[k] * coIm[k] + inIm[k] * coRe[k]
      }
    }

    // 4. Inverse FFT. RealFFT.inverse multiplies by
    //    `length = 2N`, but `coeffsF` was pre-divided by `2N` in init,
    //    so the net result is the un-normalised linear convolution
    //    sum.
    fft.inverse(specRe: tempRe, specIm: tempIm, realOut: outputBuf)

    // 5. Overlap-save output: out[i] = ifft[i] + overlap_prev[i] for
    //    i in 0..<N; overlap_next = ifft[N..2N].
    wBase.update(from: outputBuf, count: count)
    if count > 0 {
      vDSP_vaddD(
        wBase, 1, overlap, 1, wBase, 1, vDSP_Length(count))
    }
    overlap.update(from: outputBuf + chunkSize, count: chunkSize)
  }

  /// Pre-scale and FFT each IR segment into split-complex spectrum
  /// storage. Static so it's reusable from both `init` and
  /// `updateCoefficients`.
  private static func fftCoefficients(
    _ coefficients: [Double],
    chunkSize: Int,
    nsegments: Int,
    fft: RealFFT,
    coeffsFRe: UnsafeMutablePointer<Double>,
    coeffsFIm: UnsafeMutablePointer<Double>
  ) {
    let bins = chunkSize + 1
    let fftSize = 2 * chunkSize
    let invScale: Double = 1.0 / Double(fftSize)

    let scratch = UnsafeMutablePointer<Double>.allocate(capacity: fftSize)
    scratch.initialize(repeating: 0, count: fftSize)
    defer {
      scratch.deinitialize(count: fftSize)
      scratch.deallocate()
    }

    coefficients.withUnsafeBufferPointer { coeffPtr in
      guard let cBase = coeffPtr.baseAddress else { return }
      for seg in 0..<nsegments {
        let start = seg * chunkSize
        let end = min(start + chunkSize, coefficients.count)
        let n = end - start
        // Scale-and-copy into the first half; zero the rest.
        var scaled = invScale
        vDSP_vsmulD(cBase + start, 1, &scaled, scratch, 1, vDSP_Length(n))
        if n < chunkSize {
          vDSP_vclrD(scratch + n, 1, vDSP_Length(chunkSize - n))
        }
        vDSP_vclrD(scratch + chunkSize, 1, vDSP_Length(chunkSize))
        fft.forward(
          realIn: scratch,
          specRe: coeffsFRe + seg * bins,
          specIm: coeffsFIm + seg * bins)
      }
    }
  }

}
