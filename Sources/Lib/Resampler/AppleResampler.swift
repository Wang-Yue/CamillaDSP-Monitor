// CamillaDSP-Swift: Apple AudioConverter resampler.

import Accelerate
import AudioToolbox
import DSPAudio
import DSPConfig
import DSPLogging
import Foundation

final class AppleResampler: AudioResampler {
  let channels: Int
  let chunkSize: Int

  private let baseRatio: Double
  private var currentRatio: Double
  private var relativeRatioWarningEmitted = false
  private let logger = Logger(label: "camilladsp.resampler.apple")

  private var converter: AudioConverterRef? = nil
  fileprivate let fillContext: FillContext
  private let ablStorage: UnsafeMutableRawPointer

  let maxOutputFrames: Int

  var ratio: Double { currentRatio }

  private var nextOutputFrames: Int {
    let raw = Double(chunkSize) * currentRatio
    return Int(raw.rounded(.down))
  }

  fileprivate final class FillContext {
    let buffers: AudioBuffers
    var readOffset: Int = 0
    var writeOffset: Int = 0

    init(channels: Int, capacity: Int) {
      self.buffers = AudioBuffers(channels: channels, capacity: capacity)
    }
  }

  init(
    channels: Int, inputRate: Int, outputRate: Int,
    quality: AppleResamplerQuality = .max,
    complexity: AppleResamplerComplexity = .normal,
    chunkSize: Int
  ) throws {
    guard channels > 0 else {
      throw ResamplerError.invalidParameter(message: "channels must be positive, got \(channels)")
    }
    guard chunkSize > 0 else {
      throw ResamplerError.invalidParameter(message: "chunkSize must be positive, got \(chunkSize)")
    }

    self.channels = channels
    self.chunkSize = chunkSize
    self.baseRatio = Double(outputRate) / Double(inputRate)
    self.currentRatio = self.baseRatio

    let maxRelativeRatio = 1.1
    let maxRatioAbs = self.baseRatio * maxRelativeRatio
    self.maxOutputFrames = Int((Double(chunkSize) * maxRatioAbs).rounded(.up)) + 32

    self.fillContext = FillContext(channels: channels, capacity: chunkSize * 8)

    let storageSize =
      MemoryLayout<AudioBufferList>.size + (channels - 1) * MemoryLayout<AudioBuffer>.size
    self.ablStorage = UnsafeMutableRawPointer.allocate(
      byteCount: storageSize, alignment: MemoryLayout<AudioBufferList>.alignment)

    var inDesc = AudioStreamBasicDescription(
      mSampleRate: Double(inputRate),
      mFormatID: kAudioFormatLinearPCM,
      mFormatFlags: kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
        | kAudioFormatFlagIsNonInterleaved,
      mBytesPerPacket: UInt32(MemoryLayout<Double>.size),
      mFramesPerPacket: 1,
      mBytesPerFrame: UInt32(MemoryLayout<Double>.size),
      mChannelsPerFrame: UInt32(channels),
      mBitsPerChannel: UInt32(MemoryLayout<Double>.size * 8),
      mReserved: 0
    )

    var outDesc = AudioStreamBasicDescription(
      mSampleRate: Double(outputRate),
      mFormatID: kAudioFormatLinearPCM,
      mFormatFlags: kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
        | kAudioFormatFlagIsNonInterleaved,
      mBytesPerPacket: UInt32(MemoryLayout<Double>.size),
      mFramesPerPacket: 1,
      mBytesPerFrame: UInt32(MemoryLayout<Double>.size),
      mChannelsPerFrame: UInt32(channels),
      mBitsPerChannel: UInt32(MemoryLayout<Double>.size * 8),
      mReserved: 0
    )

    var conv: AudioConverterRef? = nil
    let status = AudioConverterNew(&inDesc, &outDesc, &conv)
    guard status == noErr, let converterRef = conv else {
      ablStorage.deallocate()
      throw ResamplerError.initializationFailed(
        message: "AudioConverterNew returned OSStatus \(status)")
    }
    self.converter = converterRef

    var qualityVal: UInt32 = UInt32(kAudioConverterQuality_Max)
    switch quality {
    case .min: qualityVal = UInt32(kAudioConverterQuality_Min)
    case .low: qualityVal = UInt32(kAudioConverterQuality_Low)
    case .medium: qualityVal = UInt32(kAudioConverterQuality_Medium)
    case .high: qualityVal = UInt32(kAudioConverterQuality_High)
    case .max: qualityVal = UInt32(kAudioConverterQuality_Max)
    }

    AudioConverterSetProperty(
      converterRef,
      kAudioConverterSampleRateConverterQuality,
      UInt32(MemoryLayout<UInt32>.size),
      &qualityVal
    )

    var complexityVal: UInt32 = complexity.osType
    AudioConverterSetProperty(
      converterRef,
      kAudioConverterSampleRateConverterComplexity,
      UInt32(MemoryLayout<UInt32>.size),
      &complexityVal
    )
  }

  deinit {
    if let conv = converter {
      AudioConverterDispose(conv)
    }
    ablStorage.deallocate()
  }

  /// `AppleResampler` runs at a fixed rational ratio fixed at construction.
  /// Apple's `AudioConverter` (in both default/mastering and minimum phase complexities)
  /// does not support changing the `kAudioConverterPropertyOutputSampleRate` property
  /// dynamically on an active converter (returns `kAudioConverterErr_PropertyNotSupported`).
  /// We accept the multiplier without effect and log a warning once.
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
    if output.frames < nextOutputFrames {
      throw ResamplerError.outputBufferTooSmall(needed: nextOutputFrames, got: output.frames)
    }

    guard let conv = converter else { return }

    // Check if we have space in ringBuffers
    let availableSpace = fillContext.buffers.capacity - fillContext.writeOffset
    if availableSpace < chunkSize {
      // Shift data to front if needed
      if fillContext.readOffset > 0 {
        let remaining = fillContext.writeOffset - fillContext.readOffset
        for ch in 0..<channels {
          guard let base = fillContext.buffers[ch].baseAddress else {
            throw ResamplerError.invalidParameter(message: "Buffer base address is nil")
          }
          let src = base + fillContext.readOffset
          let dst = base
          memmove(dst, src, remaining * MemoryLayout<Double>.size)
        }
        fillContext.writeOffset = remaining
        fillContext.readOffset = 0
      }

      // If still not enough space, we fail.
      guard fillContext.buffers.capacity - fillContext.writeOffset >= chunkSize else {
        fatalError("Ring buffer overflow")
      }
    }

    // Copy input into ringBuffers
    for ch in 0..<channels {
      guard let src = input[ch].baseAddress,
        let dst = fillContext.buffers[ch].baseAddress
      else {
        throw ResamplerError.invalidParameter(message: "Buffer base address is nil")
      }
      (dst + fillContext.writeOffset).update(from: src, count: chunkSize)
    }
    fillContext.writeOffset += chunkSize

    // Only call AudioConverter if we have accumulated enough data.
    // For 192->44.1, we need ~1270 frames + latency.
    // Let's use a threshold of 4096 frames.
    guard fillContext.writeOffset >= 4096 else {
      output.validFrames = 0
      return
    }

    let abl = ablStorage.bindMemory(to: AudioBufferList.self, capacity: 1)
    abl.pointee.mNumberBuffers = UInt32(channels)

    let ablBuffers = UnsafeMutableAudioBufferListPointer(abl)
    for ch in 0..<channels {
      if let base = output[ch].baseAddress {
        ablBuffers[ch].mData = UnsafeMutableRawPointer(mutating: base)
        ablBuffers[ch].mDataByteSize = UInt32(output.frames * MemoryLayout<Double>.size)
        ablBuffers[ch].mNumberChannels = 1
      }
    }

    var outputPacketCount = UInt32(output.frames)

    let userData = Unmanaged<FillContext>.passUnretained(fillContext).toOpaque()

    let status = AudioConverterFillComplexBuffer(
      conv,
      inputDataProc,
      userData,
      &outputPacketCount,
      abl,
      nil
    )

    _ = status
    output.validFrames = Int(outputPacketCount)

    // Shift remaining data to front after processing
    if fillContext.readOffset > 0 {
      let remaining = fillContext.writeOffset - fillContext.readOffset
      if remaining > 0 {
        for ch in 0..<channels {
          guard let base = fillContext.buffers[ch].baseAddress else {
            throw ResamplerError.invalidParameter(message: "Buffer base address is nil")
          }
          let src = base + fillContext.readOffset
          let dst = base
          memmove(dst, src, remaining * MemoryLayout<Double>.size)
        }
      }
      fillContext.writeOffset = remaining
      fillContext.readOffset = 0
    }
  }
}

private func inputDataProc(
  _: AudioConverterRef,
  ioNumberDataPackets: UnsafeMutablePointer<UInt32>,
  ioData: UnsafeMutablePointer<AudioBufferList>,
  _: UnsafeMutablePointer<
    UnsafeMutablePointer<AudioStreamPacketDescription>?
  >?,
  inUserData: UnsafeMutableRawPointer?
) -> OSStatus {
  guard let inUserData = inUserData else {
    ioNumberDataPackets.pointee = 0
    return noErr
  }
  let context = Unmanaged<AppleResampler.FillContext>.fromOpaque(inUserData).takeUnretainedValue()

  let needed = Int(ioNumberDataPackets.pointee)
  let available = context.writeOffset - context.readOffset

  if available <= 0 {
    ioNumberDataPackets.pointee = 0
    return noErr
  }

  let framesToProvide = min(needed, available)
  ioNumberDataPackets.pointee = UInt32(framesToProvide)

  let ablPtr = UnsafeMutableAudioBufferListPointer(ioData)
  let chans = context.buffers.channels

  for ch in 0..<min(ablPtr.count, chans) {
    guard let base = context.buffers[ch].baseAddress else {
      return -1
    }
    let shiftedBase = base + context.readOffset
    ablPtr[ch].mData = UnsafeMutableRawPointer(mutating: shiftedBase)
    ablPtr[ch].mDataByteSize = UInt32(framesToProvide * MemoryLayout<Double>.size)
    ablPtr[ch].mNumberChannels = 1
  }

  context.readOffset += framesToProvide
  return noErr
}
