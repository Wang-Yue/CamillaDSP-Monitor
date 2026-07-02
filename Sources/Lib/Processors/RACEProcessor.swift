import DSPAudio
import DSPConfig
import DSPFilters
import Foundation

final class RACEProcessor: Processor {
  let name: String
  private var channelA: Int
  private var channelB: Int

  private var delayA: DelayFilter
  private var delayB: DelayFilter
  private var gain: GainFilter

  private var feedbackA: PrcFmt = 0.0
  private var feedbackB: PrcFmt = 0.0

  init(name: String = "race", parameters: RACEParameters, sampleRate: Int) throws {
    self.name = name
    self.channelA = min(parameters.channelA, parameters.channelB)
    self.channelB = max(parameters.channelA, parameters.channelB)

    let delayParams = Self.delayConfig(parameters, sampleRate: sampleRate)
    self.delayA = DelayFilter(
      name: "\(name)-DelayA", parameters: delayParams, sampleRate: sampleRate)
    self.delayB = DelayFilter(
      name: "\(name)-DelayB", parameters: delayParams, sampleRate: sampleRate)

    let gainParams = Self.gainConfig(parameters)
    self.gain = GainFilter(name: "\(name)-Gain", parameters: gainParams)
  }

  private static func delayConfig(_ config: RACEParameters, sampleRate: Int) -> DelayParameters {
    let unit = config.delayUnitValue()
    let samplePeriod: Double
    switch unit {
    case .us:
      samplePeriod = 1_000_000.0 / Double(sampleRate)
    case .ms:
      samplePeriod = 1000.0 / Double(sampleRate)
    case .mm:
      samplePeriod = 343.0 * 1000.0 / Double(sampleRate)
    case .samples:
      samplePeriod = 1.0
    }
    let compensatedDelay = max(config.delay - samplePeriod, 0.0)
    return DelayParameters(
      delay: compensatedDelay, unit: unit, subsample: config.subsampleDelayValue())
  }

  private static func gainConfig(_ config: RACEParameters) -> GainParameters {
    return GainParameters(gain: -config.attenuation, scale: .dB, inverted: true, mute: false)
  }

  func process(chunk: inout AudioChunk) throws {
    let channelA = chunk[self.channelA]
    let channelB = chunk[self.channelB]
    let count = chunk.validFrames

    guard count > 0 else { return }

    for i in 0..<count {
      let valA = channelA[i]
      let valB = channelB[i]

      let addedA = valA + feedbackB
      let addedB = valB + feedbackA

      feedbackA = delayA.processSingle(addedA)
      feedbackB = delayB.processSingle(addedB)

      feedbackA = gain.processSingle(feedbackA)
      feedbackB = gain.processSingle(feedbackB)

      channelA[i] = addedA
      channelB[i] = addedB
    }
  }

  func updateParameters(_ config: ProcessorConfig, sampleRate: Int) {
    guard case .race(let p) = config else { return }
    self.channelA = min(p.channelA, p.channelB)
    self.channelB = max(p.channelA, p.channelB)

    let delayParams = Self.delayConfig(p, sampleRate: sampleRate)
    self.delayA.updateParameters(.delay(delayParams), sampleRate: sampleRate)
    self.delayB.updateParameters(.delay(delayParams), sampleRate: sampleRate)

    let gainParams = Self.gainConfig(p)
    self.gain.updateParameters(.gain(gainParams), sampleRate: sampleRate)
  }
}
