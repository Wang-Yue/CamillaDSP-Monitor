import DSPConfig
import Foundation

final class LookaheadLimiterFilter: Filter {
  let name: String
  private var limit: Double
  private var attackSamples: Int
  private var releaseCoeff: Double

  // Inlined LookaheadBuffer
  private var lookaheadData: AudioThreadScratchBuffer
  private var lookaheadCapacity: Int
  private var lookaheadReadIndex: Int = 0
  private var lookaheadWriteIndex: Int = 0

  private var releaseGain: Double = 1.0

  // Pre-allocated output buffer to avoid heap allocation on the hot path
  private var outputBuffer: AudioThreadScratchBuffer
  private var outputBufferCapacity: Int

  init(
    name: String = "lookahead_limiter", parameters: LookaheadLimiterParameters, sampleRate: Int,
    chunkSize: Int
  ) {
    self.name = name
    let (limit, attackSamples, releaseCoeff) = Self.configure(
      params: parameters, sampleRate: sampleRate)
    self.limit = limit
    self.attackSamples = attackSamples
    self.releaseCoeff = releaseCoeff

    let lookaheadBufferLen = max(sampleRate, chunkSize)
    self.lookaheadCapacity = lookaheadBufferLen
    self.lookaheadData = AudioThreadScratchBuffer(capacity: lookaheadBufferLen, repeating: 0.0)

    self.outputBufferCapacity = chunkSize
    self.outputBuffer = AudioThreadScratchBuffer(capacity: chunkSize, repeating: 0.0)
  }

  private static func configure(params: LookaheadLimiterParameters, sampleRate: Int) -> (
    Double, Int, Double
  ) {
    let limit = Double.fromDB(params.limit)
    let unit = params.unit ?? .ms
    let attackSamples = Int(
      computeDelaySamples(delay: params.attack, unit: unit, sampleRate: sampleRate).rounded())
    let releaseSamples = computeDelaySamples(
      delay: params.release, unit: unit, sampleRate: sampleRate)
    let releaseCoeff = exp(-1.0 / releaseSamples)
    return (limit, attackSamples, releaseCoeff)
  }

  private static func computeDelaySamples(delay: Double, unit: DelayUnit, sampleRate: Int) -> Double
  {
    switch unit {
    case .ms:
      return delay / 1000.0 * Double(sampleRate)
    case .us:
      return delay / 1_000_000.0 * Double(sampleRate)
    case .samples:
      return delay
    case .mm:
      return delay / 1000.0 * Double(sampleRate) / 343.0
    }
  }

  @inline(__always)
  private func pushOverwrite(_ sample: Double) {
    lookaheadData[lookaheadWriteIndex] = sample
    lookaheadWriteIndex = (lookaheadWriteIndex + 1) % lookaheadCapacity
    lookaheadReadIndex = (lookaheadReadIndex + 1) % lookaheadCapacity
  }

  @inline(__always)
  private func getOccupied(at idx: Int) -> Double {
    let realIdx = (lookaheadReadIndex + idx) % lookaheadCapacity
    return lookaheadData[realIdx]
  }

  func process(waveform: MutableWaveform) {
    let len = waveform.count
    if len == 0 { return }
    guard let waveBase = waveform.baseAddress else { return }

    precondition(
      len <= outputBufferCapacity,
      "Chunk size \(len) exceeds allocated capacity \(outputBufferCapacity)")

    let lookaheadStart = lookaheadCapacity - attackSamples

    // Backward pass
    var peak = 1.0
    var samplesSincePeak = attackSamples + 1

    for i in (0..<(attackSamples + len)).reversed() {
      let inputSample: Double
      if i < attackSamples {
        inputSample = getOccupied(at: lookaheadStart + i)
      } else {
        inputSample = waveBase[i - attackSamples]
      }

      let amplitude = abs(inputSample)
      var gain = amplitude > limit ? limit / amplitude : 1.0

      var rampGain = 1.0
      if samplesSincePeak <= attackSamples {
        let ramp = Double(attackSamples - samplesSincePeak) / Double(max(1, attackSamples))
        rampGain = 1.0 - (ramp * (1.0 - peak))
        samplesSincePeak += 1
      }

      if gain < rampGain {
        peak = gain
        samplesSincePeak = 1
      } else {
        gain = rampGain
      }

      if i < len {
        outputBuffer[i] = gain
      }
    }

    // Forward pass
    for i in 0..<len {
      releaseGain = pow(releaseGain, releaseCoeff)
      if outputBuffer[i] < releaseGain {
        releaseGain = outputBuffer[i]
      } else {
        outputBuffer[i] = releaseGain
      }
    }

    // Apply gain reduction
    for i in 0..<len {
      let inputSample: Double
      if i < attackSamples {
        inputSample = getOccupied(at: lookaheadStart + i)
      } else {
        inputSample = waveBase[i - attackSamples]
      }
      outputBuffer[i] *= inputSample
    }

    // Update lookahead buffer
    for i in 0..<len {
      pushOverwrite(waveBase[i])
    }

    // Output
    waveBase.update(from: outputBuffer, count: len)
  }
}
