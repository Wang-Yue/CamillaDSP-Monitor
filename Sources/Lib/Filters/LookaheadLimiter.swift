import DSPAudio
import DSPConfig
import Foundation

private struct LookaheadBuffer {
  private var data: [PrcFmt]
  private var readIndex: Int = 0
  private var writeIndex: Int = 0
  private var count: Int = 0

  init(capacity: Int) {
    self.data = [PrcFmt](repeating: 0.0, count: capacity)
    self.count = capacity
    self.readIndex = 0
    self.writeIndex = 0
  }

  mutating func pushOverwrite(_ sample: PrcFmt) {
    data[writeIndex] = sample
    writeIndex = (writeIndex + 1) % data.count
    readIndex = (readIndex + 1) % data.count
  }

  mutating func pushSliceOverwrite(_ slice: MutableWaveform) {
    for val in slice {
      pushOverwrite(val)
    }
  }

  func getOccupied(at idx: Int) -> PrcFmt {
    let realIdx = (readIndex + idx) % data.count
    return data[realIdx]
  }

  var occupiedLen: Int { count }
}

final class LookaheadLimiterFilter: Filter {
  let name: String
  private var limit: PrcFmt
  private var attackSamples: Int
  private var releaseCoeff: PrcFmt
  private var lookaheadBuffer: LookaheadBuffer
  private var releaseGain: PrcFmt = 1.0
  private var outputBuffer: [PrcFmt]

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
    self.lookaheadBuffer = LookaheadBuffer(capacity: lookaheadBufferLen)
    self.outputBuffer = [PrcFmt](repeating: 0.0, count: chunkSize)
  }

  private static func configure(params: LookaheadLimiterParameters, sampleRate: Int) -> (
    PrcFmt, Int, PrcFmt
  ) {
    let limit = PrcFmt.fromDB(params.limit)
    let unit = params.unit ?? .ms
    let attackSamples = Int(
      computeDelaySamples(delay: params.attack, unit: unit, sampleRate: sampleRate).rounded())
    let releaseSamples = computeDelaySamples(
      delay: params.release, unit: unit, sampleRate: sampleRate)
    let releaseCoeff = exp(-1.0 / releaseSamples)
    return (limit, attackSamples, releaseCoeff)
  }

  private static func computeDelaySamples(delay: PrcFmt, unit: DelayUnit, sampleRate: Int) -> PrcFmt
  {
    switch unit {
    case .ms:
      return delay / 1000.0 * PrcFmt(sampleRate)
    case .us:
      return delay / 1_000_000.0 * PrcFmt(sampleRate)
    case .samples:
      return delay
    case .mm:
      return delay / 1000.0 * PrcFmt(sampleRate) / 343.0
    }
  }

  func process(waveform: MutableWaveform) {
    let len = waveform.count
    if len == 0 { return }

    if outputBuffer.count < len {
      outputBuffer = [PrcFmt](repeating: 0.0, count: len)
    }

    let lookaheadStart = lookaheadBuffer.occupiedLen - attackSamples
    let getInputSample = { [lookaheadBuffer = self.lookaheadBuffer] (i: Int) -> PrcFmt in
      if i < self.attackSamples {
        return lookaheadBuffer.getOccupied(at: lookaheadStart + i)
      } else {
        return waveform[i - self.attackSamples]
      }
    }

    // Backward pass
    var peak = 1.0
    var samplesSincePeak = attackSamples + 1

    for i in (0..<(attackSamples + len)).reversed() {
      let amplitude = abs(getInputSample(i))
      var gain = amplitude > limit ? limit / amplitude : 1.0

      var rampGain = 1.0
      if samplesSincePeak <= attackSamples {
        let ramp = PrcFmt(attackSamples - samplesSincePeak) / PrcFmt(max(1, attackSamples))
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
      outputBuffer[i] *= getInputSample(i)
    }

    // Update lookahead buffer
    lookaheadBuffer.pushSliceOverwrite(waveform)

    // Output
    guard let waveBase = waveform.baseAddress else { return }
    waveBase.update(from: outputBuffer, count: len)
  }

  func updateParameters(_ config: FilterConfig, sampleRate: Int) {
    guard case .lookaheadLimiter(let params) = config else { return }
    let (limit, attackSamples, releaseCoeff) = Self.configure(
      params: params, sampleRate: sampleRate)
    self.limit = limit
    self.attackSamples = attackSamples
    self.releaseCoeff = releaseCoeff

    for _ in 0..<attackSamples {
      self.lookaheadBuffer.pushOverwrite(0.0)
    }
  }
}
