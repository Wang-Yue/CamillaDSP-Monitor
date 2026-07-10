import Accelerate
import DSPConfig
import Foundation

final class NoiseGateProcessor: Processor {
  let name: String
  private var monitorChannels: [Int]
  private var processChannels: [Int]
  private var attack: Double
  private var release: Double
  private var threshold: Double
  private var factor: Double
  private var scratch: AudioThreadScratchBuffer
  private var prevLoudness: Double = 0.0

  init(name: String = "noisegate", parameters: NoiseGateParameters, sampleRate: Int, chunkSize: Int)
  {
    self.name = name
    self.scratch = AudioThreadScratchBuffer(capacity: chunkSize, repeating: 0.0)

    var monitor = parameters.monitorChannelsArray()
    if monitor.isEmpty {
      monitor = Array(0..<parameters.channels)
    }
    self.monitorChannels = monitor

    var process = parameters.processChannelsArray()
    if process.isEmpty {
      process = Array(0..<parameters.channels)
    }
    self.processChannels = process

    let srate = Double(sampleRate)
    self.attack = exp(-1.0 / srate / parameters.attack)
    self.release = exp(-1.0 / srate / parameters.release)
    self.threshold = parameters.threshold
    self.factor = Double.fromDB(-parameters.attenuation)
  }

  private func calculateLinearGain(scratch: AudioThreadScratchBuffer, count: Int) {
    for i in 0..<count {
      if scratch[i] < threshold {
        scratch[i] = factor
      } else {
        scratch[i] = 1.0
      }
    }
  }

  func process(chunk: inout AudioChunk) throws {
    let count = chunk.validFrames
    let processCount = min(count, scratch.capacity)
    guard processCount > 0 else { return }

    chunk.sumChannels(monitorChannels, into: scratch, count: processCount)
    var prev = prevLoudness
    for i in 0..<processCount {
      let val = 20.0 * log10(abs(scratch[i]) + 1e-9)
      prev = Double.smoothEnvelope(val, prev: prev, attack: attack, release: release)
      scratch[i] = prev
    }
    prevLoudness = prev
    calculateLinearGain(scratch: scratch, count: processCount)

    chunk.applyGain(to: processChannels, from: scratch, count: processCount)
  }

  func transferState(from src: Processor) {
    guard let srcGate = src as? NoiseGateProcessor else { return }
    self.prevLoudness = srcGate.prevLoudness
  }
}
