// Public actor exposed to DSPMonitor.
//
// The Monitor app was originally written against the UniFFI-generated
// bindings of the Rust CamillaDSP library; this actor preserves that exact
// API surface (`start(configJson:)`, `getSpectrum`, `getVuLevels`, etc.) so
// the Monitor sources compile unchanged. Internally it drives a
// `DSPEngineCore` plus a `SpectrumAnalyzer` and bridges between the two
// vocabularies.

import DSPConfig
import Foundation
import SwiftDSP

// MARK: - The actor

public actor DSPEngine {
  public static let isRustEngine = false

  private let engine = SwiftDSPEngine()

  public init() {
    // Logging.bootstrap is a process-global hook; the Monitor app sets
    // its own handler so we deliberately don't touch it here.
  }

  // MARK: Lifecycle

  public func start(configJson: String) async throws {
    try await engine.setConfig(json: configJson)
  }

  public func stop() async {
    await engine.stop()
  }

  public func setFaderVolume(_ fader: Fader, _ db: Float) async {
    await engine.setFaderVolume(fader, db)
  }

  public func setFaderMute(_ fader: Fader, _ mute: Bool) async {
    await engine.setFaderMute(fader, mute)
  }

  public func getStatus() async -> StateUpdate {
    return await engine.getStatus()
  }

  public func getVuLevels() async -> VuLevels {
    return await engine.getVuLevels()
  }

  public func getSpectrum(
    isCapture: Bool,
    channel: UInt32?,
    minFreq: Double,
    maxFreq: Double,
    nBins: UInt32
  ) async throws -> Spectrum {
    return try await engine.getSpectrum(
      isCapture: isCapture, channel: channel, minFreq: minFreq, maxFreq: maxFreq, nBins: nBins)
  }

  public func getSamples(isCapture: Bool, nFrames: UInt32) async throws -> AudioSamples {
    return try await engine.getSamples(isCapture: isCapture, nFrames: nFrames)
  }

  public func getAvailableDevices(backend: String, input: Bool) async -> [AudioDevice] {
    return await engine.getAvailableDevices(backend: backend, input: input)
  }

  public func getDeviceCapabilities(
    backend: String,
    device: String,
    isCapture: Bool
  ) async -> AudioDeviceDescriptor? {
    return await engine.getDeviceCapabilities(
      backend: backend, device: device, isCapture: isCapture)
  }

  public func setLogLevel(_ level: LogLevel) async {
    await engine.setLogLevel(level)
  }
}
