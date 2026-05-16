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
//     scale, so the Rust version pre-divides coefficients by
//     `2 * data_length` to compensate.
//   - All hot-path buffers are owned by raw `UnsafeMutablePointer`s
//     (`AudioBuffers`-style) so `process(waveform:)` cannot trip
//     Swift's Array CoW path that a `[PrcFmt]` field would.

import Accelerate
import DSPAudio
import DSPConfig
import DSPFFT
import Foundation

/// Source format for the impulse response. Mirrors
/// `config::ConvParameters` in the Rust upstream:
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
  public func loadCoefficients(sampleRate: Int) throws -> [PrcFmt] {
    switch type {
    case .values:
      return values ?? []
    case .dummy:
      var v = [PrcFmt](repeating: 0, count: length ?? 0)
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
      return try ConvCoefficientLoader.loadRaw(path: resolved, format: format ?? "FLOAT64")
    }
  }
}

/// Coefficient file readers. Off the audio thread — straightforward
/// `Data`-based parsers, no streaming or memory-mapping.
public enum ConvCoefficientLoader {
  public static func loadWAV(path: String, channel: Int) throws -> [PrcFmt] {
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
    var result = [PrcFmt](repeating: 0, count: numFrames)
    let headerSize = 44

    for frame in 0..<numFrames {
      let offset = headerSize + (frame * Int(numChannels) + channel) * bytesPerSample
      guard offset + bytesPerSample <= data.count else { break }
      switch bitsPerSample {
      case 16:
        let raw = data.withUnsafeBytes { $0.load(fromByteOffset: offset, as: Int16.self) }
        result[frame] = PrcFmt(raw) / PrcFmt(Int16.max)
      case 24:
        let b0 = Int32(data[offset])
        let b1 = Int32(data[offset + 1])
        let b2 = Int32(data[offset + 2])
        var raw = b0 | (b1 << 8) | (b2 << 16)
        if raw & 0x800000 != 0 { raw |= -0x800000 }
        result[frame] = PrcFmt(raw) / PrcFmt((1 << 23) - 1)
      case 32:
        let raw = data.withUnsafeBytes { $0.load(fromByteOffset: offset, as: Float.self) }
        result[frame] = PrcFmt(raw)
      case 64:
        let raw = data.withUnsafeBytes { $0.load(fromByteOffset: offset, as: Double.self) }
        result[frame] = PrcFmt(raw)
      default:
        throw ConfigError.invalidFilter("Unsupported WAV bit depth: \(bitsPerSample)")
      }
    }
    return result
  }

  public static func loadRaw(path: String, format: String) throws -> [PrcFmt] {
    let url = URL(fileURLWithPath: path)
    guard FileManager.default.fileExists(atPath: path) else {
      throw ConfigError.invalidFilter("Raw file not found: \(path)")
    }

    if format == "TEXT" {
      let text = try String(contentsOf: url, encoding: .utf8)
      return text.split(separator: "\n").compactMap {
        PrcFmt($0.trimmingCharacters(in: .whitespaces))
      }
    }

    let data = try Data(contentsOf: url)
    switch format {
    case "FLOAT64", "F64_LE":
      let count = data.count / 8
      return data.withUnsafeBytes { buf in
        (0..<count).map { PrcFmt(buf.load(fromByteOffset: $0 * 8, as: Double.self)) }
      }
    case "FLOAT32", "F32_LE":
      let count = data.count / 4
      return data.withUnsafeBytes { buf in
        (0..<count).map { PrcFmt(buf.load(fromByteOffset: $0 * 4, as: Float.self)) }
      }
    case "S32_LE":
      let count = data.count / 4
      let scale = 1.0 / PrcFmt(Int32.max)
      return data.withUnsafeBytes { buf in
        (0..<count).map { PrcFmt(buf.load(fromByteOffset: $0 * 4, as: Int32.self)) * scale }
      }
    case "S16_LE":
      let count = data.count / 2
      let scale = 1.0 / PrcFmt(Int16.max)
      return data.withUnsafeBytes { buf in
        (0..<count).map { PrcFmt(buf.load(fromByteOffset: $0 * 2, as: Int16.self)) * scale }
      }
    default:
      throw ConfigError.invalidFilter("Unsupported raw format: \(format)")
    }
  }
}

final class ConvolutionFilter: Filter {

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
  private let inputBuf: UnsafeMutablePointer<PrcFmt>
  private let outputBuf: UnsafeMutablePointer<PrcFmt>

  /// Overlap-save state, length `N` — the second half of the previous
  /// IFFT result, summed into the next block's first half.
  private let overlap: UnsafeMutablePointer<PrcFmt>

  // Pre-FFT'd IR segments and rolling input-spectrum history. Each is a
  // flat `nsegments * bins` block of `PrcFmt`; the per-segment slice for
  // segment `s` lives at `[s * bins ..< (s + 1) * bins]`.
  private var coeffsFRe: UnsafeMutablePointer<PrcFmt>
  private var coeffsFIm: UnsafeMutablePointer<PrcFmt>
  private var inputFRe: UnsafeMutablePointer<PrcFmt>
  private var inputFIm: UnsafeMutablePointer<PrcFmt>

  /// Per-call accumulator for `Σ_seg input_F[hist] · coeffs_F[seg]`.
  private let tempRe: UnsafeMutablePointer<PrcFmt>
  private let tempIm: UnsafeMutablePointer<PrcFmt>

  /// Build a convolution filter from raw IR samples.
  ///
  /// - Parameters:
  ///   - coefficients: Impulse response, in time-domain sample order.
  ///     Must be non-empty.
  ///   - chunkSize: Per-call block length `N`. Must match the
  ///     `validFrames` the pipeline will hand to `process`.
  init(coefficients: [PrcFmt], chunkSize: Int) {
    precondition(chunkSize > 0, "ConvolutionFilter: chunkSize must be > 0")
    precondition(!coefficients.isEmpty, "ConvolutionFilter: coefficients must not be empty")
    self.chunkSize = chunkSize
    self.fftSize = 2 * chunkSize
    self.bins = chunkSize + 1
    self.fft = RealFFT(length: 2 * chunkSize)

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
    parameters: ConvParameters,
    chunkSize: Int,
    sampleRate: Int
  ) throws {
    try parameters.validate()
    let coeffs = try parameters.loadCoefficients(sampleRate: sampleRate)
    guard !coeffs.isEmpty else {
      throw ConfigError.invalidFilter("Conv filter resolved to empty IR")
    }
    self.init(coefficients: coeffs, chunkSize: chunkSize)
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
    //
    //    First segment uses zvmul (writes the accumulator); subsequent
    //    segments use zvma (D = A·B + C, called in-place with C == D).
    var tempSplit = DSPDoubleSplitComplex(realp: tempRe, imagp: tempIm)
    var coSplit0 = DSPDoubleSplitComplex(realp: coeffsFRe, imagp: coeffsFIm)
    var inSplit0 = DSPDoubleSplitComplex(realp: inSlotRe, imagp: inSlotIm)
    vDSP_zvmulD(
      &inSplit0, 1, &coSplit0, 1, &tempSplit, 1,
      vDSP_Length(bins), 1)

    if nsegments > 1 {
      for seg in 1..<nsegments {
        let histIdx = (index + nsegments - seg) % nsegments
        let inRe = inputFRe + histIdx * bins
        let inIm = inputFIm + histIdx * bins
        let coRe = coeffsFRe + seg * bins
        let coIm = coeffsFIm + seg * bins
        var inSplit = DSPDoubleSplitComplex(realp: inRe, imagp: inIm)
        var coSplit = DSPDoubleSplitComplex(realp: coRe, imagp: coIm)
        vDSP_zvmaD(
          &inSplit, 1, &coSplit, 1, &tempSplit, 1, &tempSplit, 1,
          vDSP_Length(bins))
      }
    }

    // 4. Inverse FFT. RealFFT.inverse multiplies by
    //    `length = 2N`, but `coeffsF` was pre-divided by `2N` in init,
    //    so the net result is the un-normalised linear convolution
    //    sum, exactly as the Rust port produces.
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
    _ coefficients: [PrcFmt],
    chunkSize: Int,
    nsegments: Int,
    fft: RealFFT,
    coeffsFRe: UnsafeMutablePointer<PrcFmt>,
    coeffsFIm: UnsafeMutablePointer<PrcFmt>
  ) {
    let bins = chunkSize + 1
    let fftSize = 2 * chunkSize
    let invScale: PrcFmt = 1.0 / PrcFmt(fftSize)

    let scratch = UnsafeMutablePointer<PrcFmt>.allocate(capacity: fftSize)
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
