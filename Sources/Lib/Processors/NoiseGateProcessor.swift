import Accelerate
import DSPAudio
import DSPConfig
import Foundation

final class NoiseGateProcessor: Processor {
  let name: String
  private var monitorChannels: [Int]
  private var processChannels: [Int]
  private var attack: PrcFmt
  private var release: PrcFmt
  private var threshold: PrcFmt
  private var factor: PrcFmt
  private var scratch: [PrcFmt]
  private var prevLoudness: PrcFmt = 0.0

  init(name: String = "noisegate", parameters: NoiseGateParameters, sampleRate: Int, chunkSize: Int)
  {
    self.name = name
    self.scratch = [PrcFmt](repeating: 0.0, count: chunkSize)

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

    let srate = PrcFmt(sampleRate)
    self.attack = exp(-1.0 / srate / parameters.attack)
    self.release = exp(-1.0 / srate / parameters.release)
    self.threshold = parameters.threshold
    self.factor = PrcFmt.fromDB(-parameters.attenuation)
  }

  private func sumMonitorChannels(from chunk: AudioChunk) {
    let count = chunk.validFrames
    let ch0 = monitorChannels[0]
    guard let src0Base = chunk[ch0].baseAddress else { return }
    scratch.withUnsafeMutableBufferPointer { destBuf in
      guard let destBase = destBuf.baseAddress else { return }
      destBase.update(from: src0Base, count: count)
      for chIdx in 1..<monitorChannels.count {
        let ch = monitorChannels[chIdx]
        guard let srcBase = chunk[ch].baseAddress else { continue }
        vDSP_vaddD(destBase, 1, srcBase, 1, destBase, 1, vDSP_Length(count))
      }
    }
  }

  private func estimateLoudness(count: Int) {
    var prev = prevLoudness
    for i in 0..<count {
      var val = 20.0 * log10(abs(scratch[i]) + 1e-9)
      if val >= prev {
        val = attack * prev + (1.0 - attack) * val
      } else {
        val = release * prev + (1.0 - release) * val
      }
      prev = val
      scratch[i] = val
    }
    prevLoudness = prev
  }

  private func calculateLinearGain(count: Int) {
    for i in 0..<count {
      if scratch[i] < threshold {
        scratch[i] = factor
      } else {
        scratch[i] = 1.0
      }
    }
  }

  private func applyGain(to waveform: MutableWaveform, count: Int) {
    guard let waveBase = waveform.baseAddress else { return }
    scratch.withUnsafeBufferPointer { scratchBuf in
      guard let scratchBase = scratchBuf.baseAddress else { return }
      vDSP_vmulD(waveBase, 1, scratchBase, 1, waveBase, 1, vDSP_Length(count))
    }
  }

  func process(chunk: inout AudioChunk) throws {
    let count = chunk.validFrames
    guard count > 0 else { return }

    if scratch.count < count {
      scratch = [PrcFmt](repeating: 0.0, count: count)
    }

    sumMonitorChannels(from: chunk)
    estimateLoudness(count: count)
    calculateLinearGain(count: count)

    for ch in processChannels {
      let wave = chunk[ch]
      applyGain(to: wave, count: count)
    }
  }

  func updateParameters(_ config: ProcessorConfig, sampleRate: Int) {
    guard case .noiseGate(let p) = config else { return }

    var monitor = p.monitorChannelsArray()
    if monitor.isEmpty {
      monitor = Array(0..<p.channels)
    }
    self.monitorChannels = monitor

    var process = p.processChannelsArray()
    if process.isEmpty {
      process = Array(0..<p.channels)
    }
    self.processChannels = process

    let srate = PrcFmt(sampleRate)
    self.attack = exp(-1.0 / srate / p.attack)
    self.release = exp(-1.0 / srate / p.release)
    self.threshold = p.threshold
    self.factor = PrcFmt.fromDB(-p.attenuation)
  }
}
