import DSPAudio
import DSPConfig
import Foundation

/// Protocol for all multi-channel audio processors.
public protocol Processor: AnyObject {
  /// The unique name of this processor instance.
  var name: String { get }

  /// Apply the processor to all channels of `chunk` in place.
  func process(chunk: inout AudioChunk) throws

  /// Update the processor parameters dynamically.
  func updateParameters(_ config: ProcessorConfig, sampleRate: Int)
}

public enum ProcessorFactory {
  public static func create(
    name: String = "processor",
    config: ProcessorConfig,
    sampleRate: Int,
    chunkSize: Int
  ) throws -> Processor {
    try config.validate()
    switch config {
    case .compressor(let p):
      return CompressorProcessor(
        name: name, parameters: p, sampleRate: sampleRate, chunkSize: chunkSize)
    case .noiseGate(let p):
      return NoiseGateProcessor(
        name: name, parameters: p, sampleRate: sampleRate, chunkSize: chunkSize)
    case .race(let p):
      return try RACEProcessor(name: name, parameters: p, sampleRate: sampleRate)
    }
  }
}
