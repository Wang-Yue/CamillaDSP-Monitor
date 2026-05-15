// FFT-based fixed-ratio sample-rate converter.
//
// Independently derived from textbook descriptions of FFT-based rate
// conversion via overlap-add convolution and spectral resampling.
//
// References
// ----------
//   * R. E. Crochiere and L. R. Rabiner (1983), "Multirate Digital
//     Signal Processing", Prentice-Hall — §3 covers the L/M
//     decimator-interpolator structure and its block-rate FFT
//     realisation.
//   * A. V. Oppenheim and R. W. Schafer, "Discrete-Time Signal
//     Processing", Prentice-Hall — §4 (sample-rate alteration), §8.7
//     ("Overlap-Save and Overlap-Add Methods" for FFT-based linear
//     convolution), §8.8 (FFT-based fast convolution).
//   * J. O. Smith, "Digital Audio Resampling Home Page", CCRMA —
//     https://ccrma.stanford.edu/~jos/resample/ — covers FFT-based
//     bandlimited interpolation and windowed-sinc filter design.
//   * F. J. Harris (1978), "On the Use of Windows for Harmonic
//     Analysis with the Discrete Fourier Transform", Proc. IEEE
//     vol. 66 no. 1 — Blackman-Harris window (used here via
//     `WindowFunction.swift`).
//
// Algorithm
// ---------
// Given input rate `Fᵢ`, output rate `Fₒ`, and `g = gcd(Fᵢ, Fₒ)`,
// define
//
//     L = Fᵢ / g     (input block size in samples per rational period)
//     M = Fₒ / g     (output block size in samples per rational period)
//
// Any integer multiple `N = K·L` input samples corresponds to
// exactly `K·M` output samples — the resampler is fixed-ratio. We
// round the user-supplied `chunkSize` up to the smallest valid
// `K·L`, which fixes the per-call input/output block lengths.
//
// At init, build a windowed-sinc lowpass filter `h[n]` of length `N`
// with cutoff at `min(1, Fₒ/Fᵢ) · π` rad/sample (Crochiere & Rabiner
// §3.1, Smith CCRMA §"Windowed-Sinc Filter Design"), zero-pad to
// length `2N`, and pre-FFT it once into `H`.
//
// Per chunk per channel:
//
//   1. Forward 2N-point real FFT of the zero-padded input. The
//      zero-pad to length 2N converts the otherwise cyclic FFT
//      convolution into a linear convolution — the standard
//      overlap-add structure in Oppenheim & Schafer §8.7.
//
//   2. Multiply pointwise by `H` to apply the anti-aliasing filter
//      in the frequency domain. Cost: O(N) per chunk versus O(N²)
//      for a direct time-domain convolution.
//
//   3. Build the output spectrum of length `2P` where `P = K·M`:
//        — bins 0…min(N, P) get a copy of the filtered input
//          spectrum;
//        — bins above are set to zero.
//      For upsampling (M > L), the zero-pad above input Nyquist is
//      what extends the bandwidth. For downsampling (M < L),
//      truncating to the first `P + 1` unique bins is the
//      band-limiting step. This is the "spectral resampling" of
//      Smith's CCRMA notes.
//
//   4. Inverse 2P-point real FFT recovers a length-2P time-domain
//      block.
//
//   5. Overlap-add: emit `result[0..P) + carry`, save
//      `result[P..2P)` as `carry` for the next chunk
//      (Oppenheim & Schafer §8.7).
//
// The arbitrary-length real FFTs are handled by `RealFFT`,
// which lets the block lengths be sized exactly to `L` and `M`
// rather than padded to a power of two.
//
// Allocation discipline
// ---------------------
// Every per-channel and per-call buffer is allocated once at
// `init`. `process(input:into:)` does no heap allocation and writes
// directly into the caller's pre-allocated `output` chunk.

import Accelerate
import DSPAudio
import DSPFFT
import DSPLogging
import Foundation

final class SynchronousResampler: AudioResampler {
  let channels: Int
  /// Input frames the resampler expects on every `process` call —
  /// `K·L` for some integer `K ≥ 1`, where `L = Fᵢ / gcd(Fᵢ, Fₒ)`.
  let chunkSize: Int
  /// Output frames produced per `process` call — `K·M`, where
  /// `M = Fₒ / gcd(Fᵢ, Fₒ)`.
  let outputChunkSize: Int

  private let _ratio: Double

  /// Length of the working FFT block on the input side (`= chunkSize`).
  private let inputBlockLen: Int
  /// Length of the working FFT block on the output side (`= outputChunkSize`).
  private let outputBlockLen: Int
  /// Number of unique-bin frequencies common to the input and output
  /// spectra: `min(inputBlockLen, outputBlockLen) + 1`. Bins above
  /// this in the output spectrum are zeroed (band-limiting for
  /// downsampling, spectral zero-pad for upsampling).
  private let sharedBins: Int

  // Anti-aliasing filter, pre-FFT'd at init. `inputBlockLen + 1`
  // unique bins. Stored as raw UnsafeMutablePointer to bypass closure/ARC overhead.
  private let filterSpecRe: UnsafeMutablePointer<Double>
  private let filterSpecIm: UnsafeMutablePointer<Double>

  // Real-input FFT engines. The forward engine handles the zero-padded
  // input block (length `2 · inputBlockLen`); the inverse engine
  // reconstructs the output block (length `2 · outputBlockLen`).
  private let inputFFT: RealFFT
  private let outputFFT: RealFFT

  // Per-channel time-domain overlap-add carry. Each entry holds the
  // tail of the previous chunk's IFFT result, length `outputBlockLen`.
  private let carries: [UnsafeMutablePointer<Double>]

  // Hot-path scratch buffers reused across channels. Unified to minimize
  // cache footprint and avoid intermediate allocations. Raw pointer
  // storage guarantees zero closure-nesting overhead in the hot path loop.
  //   `workingTime`: holds the 2N zero-padded input block for forward FFT,
  //                  and the 2P overlap-add output block from inverse FFT.
  //   `workingSpecRe`/`Im`: holds the shared low-frequency bins during filtering.
  private let workingTime: UnsafeMutablePointer<Double>
  private let workingSpecRe: UnsafeMutablePointer<Double>
  private let workingSpecIm: UnsafeMutablePointer<Double>

  private var relativeRatioWarningEmitted = false
  private let logger = Logger(label: "camilladsp.resampler.synchronous")

  var ratio: Double { _ratio }
  var maxOutputFrames: Int { outputChunkSize }

  init(channels: Int, inputRate: Int, outputRate: Int, chunkSize requestedChunkSize: Int) {
    precondition(channels > 0, "channels must be positive")
    precondition(requestedChunkSize > 0, "chunkSize must be positive")
    precondition(inputRate > 0 && outputRate > 0, "sample rates must be positive")

    self.channels = channels
    self._ratio = Double(outputRate) / Double(inputRate)

    // Block-size selection by rational decomposition.
    //   g = gcd(Fᵢ, Fₒ);   L = Fᵢ/g;   M = Fₒ/g
    //   K·L input samples ↔ K·M output samples (exactly, for any K ≥ 1)
    // Round the requested chunkSize up to the smallest valid K·L.
    let g = Self.gcd(inputRate, outputRate)
    let L = inputRate / g
    let M = outputRate / g
    let K = max(1, Int((Double(requestedChunkSize) / Double(L)).rounded(.up)))
    let inputBlock = K * L
    let outputBlock = K * M

    self.inputBlockLen = inputBlock
    self.outputBlockLen = outputBlock
    self.chunkSize = inputBlock
    self.outputChunkSize = outputBlock
    self.sharedBins = min(inputBlock, outputBlock) + 1

    // Build the anti-aliasing kernel. The kernel is applied at input
    // rate, so all frequencies are normalised to input Nyquist. The
    // *target* — highest frequency we want passed cleanly — is input
    // Nyquist for upsampling and output Nyquist (= `Fₒ/Fᵢ` of input
    // Nyquist) for downsampling (Crochiere & Rabiner §3.1).
    let targetNyquist: Double =
      inputRate > outputRate ? Double(outputRate) / Double(inputRate) : 1.0
    let baseCutoff = cutoffForBlackmanHarris2(
      filterLength: inputBlock, targetNyquist: targetNyquist)
    let kernel = makeBlackmanHarris2SincKernel(length: inputBlock, cutoff: baseCutoff)

    // Zero-pad the unity-DC-gain kernel into a length-2N buffer for
    // overlap-add convolution (Oppenheim & Schafer §8.7). Pre-scaling
    // by 1/(2·N) folds the unnormalised forward + inverse FFT scale
    // factors into the filter so the resampler delivers unity gain to
    // its callers.
    let twoN = 2 * inputBlock
    var filterTime = [Double](repeating: 0, count: twoN)
    let scale = 1.0 / Double(twoN)
    for i in 0..<inputBlock {
      filterTime[i] = kernel[i] * scale
    }

    let inputFFT = RealFFT(length: twoN)
    let outputFFT = RealFFT(length: 2 * outputBlock)
    self.inputFFT = inputFFT
    self.outputFFT = outputFFT

    // FFT the filter once at init; only the `inputBlock + 1` unique
    // bins are stored (real-input FFT has Hermitian symmetry, so the
    // upper half is redundant).
    let fRe = UnsafeMutablePointer<Double>.allocate(capacity: inputBlock + 1)
    let fIm = UnsafeMutablePointer<Double>.allocate(capacity: inputBlock + 1)
    fRe.update(repeating: 0, count: inputBlock + 1)
    fIm.update(repeating: 0, count: inputBlock + 1)
    filterTime.withUnsafeBufferPointer { tp in
      guard let tpPtr = tp.baseAddress else { return }
      inputFFT.forward(realIn: tpPtr, specRe: fRe, specIm: fIm)
    }
    self.filterSpecRe = fRe
    self.filterSpecIm = fIm

    self.carries = (0..<channels).map { _ in
      let ptr = UnsafeMutablePointer<Double>.allocate(capacity: outputBlock)
      ptr.update(repeating: 0, count: outputBlock)
      return ptr
    }
    let maxLen = max(inputBlock, outputBlock)
    self.workingTime = .allocate(capacity: 2 * maxLen)
    self.workingSpecRe = .allocate(capacity: maxLen + 1)
    self.workingSpecIm = .allocate(capacity: maxLen + 1)
    self.workingTime.update(repeating: 0, count: 2 * maxLen)
    self.workingSpecRe.update(repeating: 0, count: maxLen + 1)
    self.workingSpecIm.update(repeating: 0, count: maxLen + 1)
  }

  deinit {
    filterSpecRe.deallocate()
    filterSpecIm.deallocate()
    workingTime.deallocate()
    workingSpecRe.deallocate()
    workingSpecIm.deallocate()
    for ptr in carries {
      ptr.deallocate()
    }
  }

  /// `SynchronousResampler` runs at a fixed rational ratio fixed at
  /// construction. The rate-adjust controller's relative multiplier
  /// has nowhere to go here — we accept it without effect, logging
  /// once on the first non-unity request so the configuration error
  /// is at least surfaced.
  func setRelativeRatio(_ multiplier: Double) {
    if !relativeRatioWarningEmitted, abs(multiplier - 1.0) > 1e-9 {
      relativeRatioWarningEmitted = true
      logger.warning("relative ratio %f ignored (fixed-ratio)", .double(multiplier))
    }
  }

  func process(input: AudioChunk, into output: inout AudioChunk) throws {
    guard input.validFrames == chunkSize else {
      throw ResamplerError.inputSizeMismatch(needed: chunkSize, got: input.validFrames)
    }
    guard output.channels == channels else {
      throw ResamplerError.channelCountMismatch(needed: channels, got: output.channels)
    }
    if output.frames < outputChunkSize {
      throw ResamplerError.outputBufferTooSmall(
        needed: outputChunkSize, got: output.frames)
    }

    for ch in 0..<channels {
      processChannel(
        input: UnsafeBufferPointer(input[ch]),
        output: output[ch],
        carry: carries[ch])
    }
    output.validFrames = outputChunkSize
  }

  /// One channel's worth of FFT-based overlap-add convolution +
  /// spectral remap. All scratch buffers are class-owned; this
  /// function performs no heap allocation.
  @inline(__always)
  private func processChannel(
    input: UnsafeBufferPointer<Double>,
    output: UnsafeMutableBufferPointer<Double>,
    carry: UnsafeMutablePointer<Double>
  ) {
    guard let srcPtr = input.baseAddress else { return }
    guard let outPtr = output.baseAddress else { return }

    // Step 1. Place the input block at the start of a length-2N
    // buffer, with the second half zero. The zero-pad is what makes
    // the cyclic FFT convolution behave as a linear convolution
    // (Oppenheim & Schafer §8.7). The upper half is cleared each call
    // to ensure the zero-pad region for the forward FFT is clean.
    workingTime.update(from: srcPtr, count: inputBlockLen)
    (workingTime + inputBlockLen).update(repeating: 0, count: inputBlockLen)

    // Step 2. Forward 2N-point real FFT.
    inputFFT.forward(realIn: workingTime, specRe: workingSpecRe, specIm: workingSpecIm)

    // Step 3. Pointwise multiply input spectrum by the pre-FFT'd
    // filter. Only the `sharedBins` matter since bins above are
    // dropped on the output side; doing the multiply in place over
    // that span avoids touching the upper half.
    var ioSplit = DSPDoubleSplitComplex(realp: workingSpecRe, imagp: workingSpecIm)
    var fSplit = DSPDoubleSplitComplex(realp: filterSpecRe, imagp: filterSpecIm)
    vDSP_zvmulD(&ioSplit, 1, &fSplit, 1, &ioSplit, 1, vDSP_Length(sharedBins), 1)

    // Step 4. Build the output spectrum of length `2·outputBlockLen`:
    // copy the filtered low bins and zero the rest. For upsampling
    // (outputBlockLen > inputBlockLen) the zeros above input Nyquist
    // are the spectral zero-pad that extends the bandwidth. For
    // downsampling they discard everything above output Nyquist —
    // the band-limiting step.
    let zeroCount = outputBlockLen + 1 - sharedBins
    if zeroCount > 0 {
      (workingSpecRe + sharedBins).update(repeating: 0, count: zeroCount)
      (workingSpecIm + sharedBins).update(repeating: 0, count: zeroCount)
    }

    // Step 5. Inverse 2P-point real FFT to time domain (P = outputBlockLen).
    outputFFT.inverse(specRe: workingSpecRe, specIm: workingSpecIm, realOut: workingTime)

    // Step 6. Overlap-add: write `result[0..P) + carry` as the chunk's
    // output samples, and save `result[P..2P)` for the next chunk's
    // overlap.
    let resultHead = UnsafeBufferPointer(start: workingTime, count: outputBlockLen)
    let carryBuf = UnsafeBufferPointer(start: carry, count: outputBlockLen)
    var outSlice = UnsafeMutableBufferPointer(start: outPtr, count: outputBlockLen)
    vDSP.add(resultHead, carryBuf, result: &outSlice)
    carry.update(from: workingTime + outputBlockLen, count: outputBlockLen)
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
