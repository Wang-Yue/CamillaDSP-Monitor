// CamillaDSP-Swift: faithful port of rubato's `Fft<f64>` (synchro.rs) running
// in `FixedSync::Both` mode with `sub_chunks = 1`. The algorithm matches the
// Rust upstream up to floating-point noise:
//
//   1. fft_size_in/out are picked from the rate ratio's GCD (chunk_size is
//      rounded up to the smallest multiple of `inputRate / gcd`).
//   2. A windowed-sinc anti-aliasing kernel (BlackmanHarris2) is built once,
//      zero-padded to length 2·fft_size_in, and pre-FFT'd into `filter_f`.
//   3. Per chunk per channel: forward FFT input (size 2·fft_size_in),
//      multiply by `filter_f`, copy/zero-pad spectrum to length 2·fft_size_out
//      with conjugate symmetry, inverse FFT, and overlap-add the second half
//      of the result into the next chunk.
//
// Because rubato's chosen FFT sizes are typically not f·2^n (e.g. 2058 for
// 44.1↔48 kHz), we use Bluestein's algorithm (`BluesteinFFT.swift`) to handle
// arbitrary lengths through vDSP's power-of-2 DFT. Compared with realfft this
// costs ~3× the inner FFTs but is still O(N log N).
//
// All buffers are pre-allocated at init from the fixed `chunkSize`. The hot
// path (`process(input:into:)`) does no allocation.

import Accelerate
import Foundation

public final class SynchronousResampler: AudioResampler {
  public let channels: Int
  /// Input frames per call — equals rubato's `fft_size_in` (= `chunk_size_in`
  /// in `FixedSync::Both` mode). The constructor rounds the user-supplied
  /// `chunkSize` up to the smallest valid multiple of `inputRate / gcd`.
  public let chunkSize: Int
  /// Output frames per call — equals rubato's `fft_size_out`.
  public let outputChunkSize: Int

  private let inputRate: Int
  private let outputRate: Int
  private let _ratio: Double

  // Algorithm params (matching rubato's variable names).
  private let fftSizeIn: Int
  private let fftSizeOut: Int
  /// `min(fft_size_in + 1, fft_size_out)` — the unique-bin span of the
  /// post-filter spectrum (rubato's `new_len`).
  private let newLen: Int

  // Pre-built filter spectrum (indices 0..fftSizeIn inclusive).
  private let filterFReal: [Double]
  private let filterFImag: [Double]

  // Real-input FFT engines: `forward` produces the `fftSize + 1` unique bins
  // and `inverse` consumes them. Internally these run an N-point complex
  // Bluestein at half the size of a generic 2N-point one — same algorithmic
  // result as `realfft`, but with vDSP doing the inner power-of-2 FFTs.
  private let realFFTIn: BluesteinRealFFT
  private let realFFTOut: BluesteinRealFFT

  // Per-channel time-domain overlap (length `fft_size_out`).
  private var overlaps: [[Double]]

  // Hot-path scratch (reused across channels).
  private var inputBufRe: [Double]  // 2 · fftSizeIn (real input, second half zero)
  private var inputFRe: [Double]  // fftSizeIn + 1 (unique bins)
  private var inputFIm: [Double]
  private var outputFRe: [Double]  // fftSizeOut + 1
  private var outputFIm: [Double]
  private var outputBufRe: [Double]  // 2 · fftSizeOut (real IFFT output)

  private var relativeRatioWarningEmitted = false

  public var ratio: Double { _ratio }
  public var nextOutputFrames: Int { outputChunkSize }
  public var maxOutputFrames: Int { outputChunkSize }

  public init(channels: Int, inputRate: Int, outputRate: Int, chunkSize requestedChunkSize: Int) {
    precondition(channels > 0, "channels must be positive")
    precondition(requestedChunkSize > 0, "chunkSize must be positive")
    precondition(inputRate > 0 && outputRate > 0, "sample rates must be positive")

    self.channels = channels
    self.inputRate = inputRate
    self.outputRate = outputRate
    self._ratio = Double(outputRate) / Double(inputRate)

    // Match rubato's `Fft::new(.., FixedSync::Both)` size pick:
    //   gcd          = gcd(inputRate, outputRate)
    //   minChunkIn   = inputRate / gcd
    //   fft_chunks   = ceil(chunk_size / minChunkIn)
    //   fft_size_in  = fft_chunks * inputRate  / gcd
    //   fft_size_out = fft_chunks * outputRate / gcd
    let g = Self.gcd(inputRate, outputRate)
    let minChunkIn = inputRate / g
    let fftChunks = max(1, Int((Double(requestedChunkSize) / Double(minChunkIn)).rounded(.up)))
    let fftSizeIn = fftChunks * (inputRate / g)
    let fftSizeOut = fftChunks * (outputRate / g)
    self.fftSizeIn = fftSizeIn
    self.fftSizeOut = fftSizeOut
    // FixedSync::Both with sub_chunks = 1 → chunk_size_{in,out} == fft_size_{in,out}.
    self.chunkSize = fftSizeIn
    self.outputChunkSize = fftSizeOut
    self.newLen = (fftSizeIn < fftSizeOut) ? (fftSizeIn + 1) : fftSizeOut

    // Build the anti-aliasing filter exactly as rubato does
    // (`synchro.rs:97-114`):
    //   cutoff = downsampling
    //          ? calculate_cutoff(fft_size_out) * fft_size_out / fft_size_in
    //          : calculate_cutoff(fft_size_in)
    //   sinc = make_sincs(fft_size_in, factor=1, cutoff, BlackmanHarris2)
    //   filter_t[n]      = sinc[0][n] / (2 * fft_size_in)   for n < fft_size_in
    //   filter_t[n>=N]   = 0
    //   filter_f         = FFT(filter_t)                      (length 2·fft_size_in)
    let cutoffF32: Float
    if fftSizeIn > fftSizeOut {
      let baseCut = calculateCutoffF32(sincLen: fftSizeOut, window: .blackmanHarris2)
      cutoffF32 = baseCut * Float(fftSizeOut) / Float(fftSizeIn)
    } else {
      cutoffF32 = calculateCutoffF32(sincLen: fftSizeIn, window: .blackmanHarris2)
    }
    let cutoff = Double(cutoffF32)
    let sinc = makeSincTable(
      sincLen: fftSizeIn, oversamplingFactor: 1, window: .blackmanHarris2, fc: cutoff)

    let twoNIn = 2 * fftSizeIn
    var filterT = [Double](repeating: 0, count: twoNIn)
    let invFilterScale = 1.0 / Double(twoNIn)
    for k in 0..<fftSizeIn {
      filterT[k] = sinc[k] * invFilterScale
    }

    let realFFTIn = BluesteinRealFFT(length: twoNIn)
    let realFFTOut = BluesteinRealFFT(length: 2 * fftSizeOut)
    self.realFFTIn = realFFTIn
    self.realFFTOut = realFFTOut

    // FFT the filter once at init. Only `fftSizeIn + 1` unique bins matter.
    var filterFReal = [Double](repeating: 0, count: fftSizeIn + 1)
    var filterFImag = [Double](repeating: 0, count: fftSizeIn + 1)
    filterT.withUnsafeBufferPointer { tr in
      guard let trPtr = tr.baseAddress else { return }
      filterFReal.withUnsafeMutableBufferPointer { fr in
        guard let frPtr = fr.baseAddress else { return }
        filterFImag.withUnsafeMutableBufferPointer { fi in
          guard let fiPtr = fi.baseAddress else { return }
          realFFTIn.forward(
            realIn: trPtr,
            specRe: frPtr, specIm: fiPtr)
        }
      }
    }
    self.filterFReal = filterFReal
    self.filterFImag = filterFImag

    self.overlaps = (0..<channels).map { _ in [Double](repeating: 0, count: fftSizeOut) }

    self.inputBufRe = [Double](repeating: 0, count: twoNIn)
    self.inputFRe = [Double](repeating: 0, count: fftSizeIn + 1)
    self.inputFIm = [Double](repeating: 0, count: fftSizeIn + 1)
    self.outputFRe = [Double](repeating: 0, count: fftSizeOut + 1)
    self.outputFIm = [Double](repeating: 0, count: fftSizeOut + 1)
    self.outputBufRe = [Double](repeating: 0, count: 2 * fftSizeOut)
  }

  public func setRelativeRatio(_ multiplier: Double) {
    if !relativeRatioWarningEmitted, abs(multiplier - 1.0) > 1e-9 {
      relativeRatioWarningEmitted = true
      print("camilladsp.resampler.synchronous: relative ratio \(multiplier) ignored (fixed-ratio)")
    }
  }

  public func process(input: AudioChunk, into output: inout AudioChunk) throws {
    guard input.validFrames == chunkSize else {
      throw ResamplerError.inputSizeMismatch(needed: chunkSize, got: input.validFrames)
    }
    guard output.waveforms.count == channels else {
      throw ResamplerError.channelCountMismatch(needed: channels, got: output.waveforms.count)
    }
    for ch in 0..<channels {
      guard output.waveforms[ch].count >= outputChunkSize else {
        throw ResamplerError.outputBufferTooSmall(
          needed: outputChunkSize, got: output.waveforms[ch].count)
      }
    }

    for ch in 0..<channels {
      resampleUnit(
        input: input.waveforms[ch], output: &output.waveforms[ch], overlap: &overlaps[ch])
    }
    output.validFrames = outputChunkSize
  }

  /// One FftResampler::resample_unit pass. Algorithmically identical to
  /// rubato's `synchro.rs:142-191`, but the inner real-FFT lets us skip the
  /// manual conjugate-symmetric reconstruction — the spectrum lives only in
  /// the `fftSize + 1` unique bins.
  @inline(__always)
  private func resampleUnit(
    input: [Double], output: inout [Double], overlap: inout [Double]
  ) {
    let twoNIn = 2 * fftSizeIn

    // 1. Copy input into the first half of `inputBufRe`, zero the second half.
    inputBufRe.withUnsafeMutableBufferPointer { dst in
      guard let dstPtr = dst.baseAddress else { return }
      input.withUnsafeBufferPointer { src in
        guard let srcPtr = src.baseAddress else { return }
        dstPtr.update(from: srcPtr, count: fftSizeIn)
      }
      (dstPtr + fftSizeIn).update(repeating: 0, count: twoNIn - fftSizeIn)
    }

    // 2. Forward real FFT — produces `fftSizeIn + 1` unique complex bins.
    inputBufRe.withUnsafeBufferPointer { tr in
      guard let trPtr = tr.baseAddress else { return }
      inputFRe.withUnsafeMutableBufferPointer { fr in
        guard let frPtr = fr.baseAddress else { return }
        inputFIm.withUnsafeMutableBufferPointer { fi in
          guard let fiPtr = fi.baseAddress else { return }
          realFFTIn.forward(
            realIn: trPtr,
            specRe: frPtr, specIm: fiPtr)
        }
      }
    }

    // 3. Multiply by the filter spectrum on the unique-bin span (in-place).
    inputFRe.withUnsafeMutableBufferPointer { iReBuf in
      guard let iRePtr = iReBuf.baseAddress else { return }
      inputFIm.withUnsafeMutableBufferPointer { iImBuf in
        guard let iImPtr = iImBuf.baseAddress else { return }
        filterFReal.withUnsafeBufferPointer { fReBuf in
          guard let fRePtr = fReBuf.baseAddress else { return }
          filterFImag.withUnsafeBufferPointer { fImBuf in
            guard let fImPtr = fImBuf.baseAddress else { return }
            var inSplit = DSPDoubleSplitComplex(realp: iRePtr, imagp: iImPtr)
            var fSplit = DSPDoubleSplitComplex(
              realp: UnsafeMutablePointer(mutating: fRePtr),
              imagp: UnsafeMutablePointer(mutating: fImPtr))
            vDSP_zvmulD(&inSplit, 1, &fSplit, 1, &inSplit, 1, vDSP_Length(newLen), 1)
          }
        }
      }
    }

    // 4. Build the output spectrum (length `fftSizeOut + 1`):
    //    [0..newLen)               = filtered input
    //    [newLen .. fftSizeOut]    = 0  (zero-pad through Nyquist)
    outputFRe.withUnsafeMutableBufferPointer { oReBuf in
      guard let oRe = oReBuf.baseAddress else { return }
      outputFIm.withUnsafeMutableBufferPointer { oImBuf in
        guard let oIm = oImBuf.baseAddress else { return }
        inputFRe.withUnsafeBufferPointer { iRe in
          guard let iRePtr = iRe.baseAddress else { return }
          inputFIm.withUnsafeBufferPointer { iIm in
            guard let iImPtr = iIm.baseAddress else { return }
            oRe.update(from: iRePtr, count: newLen)
            oIm.update(from: iImPtr, count: newLen)
          }
        }
        let zeroCount = fftSizeOut + 1 - newLen
        if zeroCount > 0 {
          (oRe + newLen).update(repeating: 0, count: zeroCount)
          (oIm + newLen).update(repeating: 0, count: zeroCount)
        }
      }
    }

    // 5. Inverse real FFT (unnormalised — matches realfft).
    outputFRe.withUnsafeBufferPointer { fr in
      guard let frPtr = fr.baseAddress else { return }
      outputFIm.withUnsafeBufferPointer { fi in
        guard let fiPtr = fi.baseAddress else { return }
        outputBufRe.withUnsafeMutableBufferPointer { br in
          guard let brPtr = br.baseAddress else { return }
          realFFTOut.inverse(
            specRe: frPtr, specIm: fiPtr,
            realOut: brPtr)
        }
      }
    }

    outputBufRe.withUnsafeBufferPointer { obBuf in
      guard let obPtr = obBuf.baseAddress else { return }
      overlap.withUnsafeBufferPointer { ovlBuf in
        guard let ovlPtr = ovlBuf.baseAddress else { return }
        output.withUnsafeMutableBufferPointer { outBuf in
          guard let outPtr = outBuf.baseAddress else { return }
          let obSub = UnsafeBufferPointer(start: obPtr, count: fftSizeOut)
          let ovlSub = UnsafeBufferPointer(start: ovlPtr, count: fftSizeOut)
          var outSub = UnsafeMutableBufferPointer(start: outPtr, count: fftSizeOut)
          vDSP.add(obSub, ovlSub, result: &outSub)
        }
      }
    }
    // 7. New overlap = second half of IFFT result.
    overlap.withUnsafeMutableBufferPointer { ovlBuf in
      guard let ovlPtr = ovlBuf.baseAddress else { return }
      outputBufRe.withUnsafeBufferPointer { obBuf in
        guard let obPtr = obBuf.baseAddress else { return }
        ovlPtr.update(from: obPtr + fftSizeOut, count: fftSizeOut)
      }
    }
  }

  private static func gcd(_ a: Int, _ b: Int) -> Int {
    var x = abs(a)
    var y = abs(b)
    while y != 0 {
      let t = y
      y = x % y
      x = t
    }
    return x
  }
}
