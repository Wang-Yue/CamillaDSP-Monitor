// Device capability discovery for CoreAudio.
//
// CamillaDSP 4.2.0 exposes a `GetCaptureDeviceCapabilities` /
// `GetPlaybackDeviceCapabilities` websocket command, but the Rust
// implementation on `next4.2.0` does not actually populate capability
// data — it only returns device names. CamillaDSP-Monitor's UI, however,
// already consumes a structured capability tree to render the device
// configuration dialog. This module fills that gap by walking the
// CoreAudio HAL directly: for each output (or input) stream of the
// requested device we enumerate every available physical format and
// translate it into the (channels, samplerate, format) tuples the UI
// expects.

import AudioToolbox
import CoreAudio
import DSPConfig
import Foundation

// MARK: - Discovery

public enum CoreAudioCapabilities {

  /// Sample rates we report when a device exposes a *range* rather than a
  /// discrete list. CoreAudio devices commonly advertise something like
  /// 44.1 kHz – 192 kHz; we report only the standard rates that fall
  /// inside the range so the UI doesn't need to render thousands of
  /// values.
  ///
  /// Public so room-correction tooling can pre-render an FIR per
  /// rate, then pick the matching one at engine-config time.
  public static let standardRates: [Int] = [
    8000, 11025, 16000, 22050, 32000,
    44100, 48000, 88200, 96000,
    176400, 192000, 352800, 384000,
    705600, 768000,
  ]

  // MARK: Device enumeration
  //
  // Thin wrappers over `CoreAudioDevice` so the UI doesn't need to
  // touch HAL types. Anything beyond a name lives in the capability
  // descriptor (`describe`) below.

  /// Names of all devices visible to the system in the requested
  /// direction. Empty when no devices match (no mics connected, no
  /// output devices, etc.).
  public static func availableDeviceNames(isCapture: Bool) -> [String] {
    let scope: CoreAudioScope = isCapture ? .input : .output
    return CoreAudioDevice.devices(scope: scope)
      .map { $0.name }
      .filter { !$0.isEmpty }
  }

  /// Name of the system-default device in the requested direction,
  /// if one is configured. Useful as the initial value for a picker.
  public static func defaultDeviceName(isCapture: Bool) -> String? {
    let scope: CoreAudioScope = isCapture ? .input : .output
    guard let id = CoreAudioDevice.defaultDeviceID(scope: scope) else { return nil }
    return CoreAudioDevice.name(of: id)
  }

  /// Maximum channel count the named device exposes across any of
  /// its physical formats. When `name` is `nil` the system default
  /// is queried. Returns `0` if the device can't be located.
  ///
  /// Derived from `describe(deviceName:isCapture:)` — no separate HAL
  /// query — so the answer matches whatever the capability descriptor
  /// reports. Used by the room-correction UI to populate per-channel
  /// pickers (e.g. left/right speaker, calibrated mic capsule on a
  /// stereo interface).
  public static func channelCount(deviceName name: String?, isCapture: Bool) -> Int {
    let resolved = name ?? defaultDeviceName(isCapture: isCapture)
    guard let resolved,
      let descriptor = describe(deviceName: resolved, isCapture: isCapture)
    else { return 0 }
    return descriptor.capability_sets
      .flatMap { $0.capabilities }
      .map { $0.channels }
      .max() ?? 0
  }

  /// Build the capability descriptor for a named device. Returns `nil`
  /// if the device cannot be located. All low-level HAL plumbing is
  /// delegated to `CoreAudioDevice`; this layer only adds the
  /// physical-format probe + aggregation that's specific to the UI's
  /// `AudioDeviceDescriptor` shape.
  public static func describe(deviceName name: String, isCapture: Bool) -> AudioDeviceDescriptor? {
    let scope: CoreAudioScope = isCapture ? .input : .output
    guard let id = CoreAudioDevice.deviceID(forName: name, scope: scope) else { return nil }
    let resolvedName = CoreAudioDevice.name(of: id) ?? name
    let streamFormats = CoreAudioDevice.streams(of: id, scope: scope).flatMap {
      availablePhysicalFormats(stream: $0)
    }
    return AudioDeviceDescriptor(
      name: resolvedName,
      capability_sets: [aggregate(formats: streamFormats)]
    )
  }

  // MARK: Aggregation

  /// Group `(channels, samplerate, format)` tuples into the nested shape
  /// the UI consumes: channels → samplerates → formats.
  static func aggregate(formats: [PhysicalFormat]) -> DeviceCapabilitySet {
    // Bucket by channel count, then by sample rate, then collect format
    // strings (deduplicated, stable order).
    var byChannel: [Int: [Int: [String]]] = [:]
    for fmt in formats {
      for rate in fmt.samplerates {
        let formatString = fmt.formatString
        guard !formatString.isEmpty else { continue }
        var rateMap = byChannel[fmt.channels] ?? [:]
        var existing = rateMap[rate] ?? []
        if !existing.contains(formatString) {
          existing.append(formatString)
        }
        rateMap[rate] = existing
        byChannel[fmt.channels] = rateMap
      }
    }

    let channels = byChannel.keys.sorted()
    let capabilities: [ChannelCapability] = channels.map { ch in
      let rateMap = byChannel[ch] ?? [:]
      let rates = rateMap.keys.sorted()
      let perRate = rates.map { rate in
        SamplerateCapability(samplerate: rate, formats: rateMap[rate] ?? [])
      }
      return ChannelCapability(channels: ch, samplerates: perRate)
    }
    return DeviceCapabilitySet(capabilities: capabilities)
  }

  // MARK: CoreAudio plumbing

  /// One physical format entry from `kAudioStreamPropertyAvailablePhysicalFormats`.
  /// `samplerates` is the list of standard rates that fit inside the
  /// AudioStreamRangedDescription range (typically a single value, but
  /// some devices report a range).
  struct PhysicalFormat {
    let channels: Int
    let samplerates: [Int]
    let formatString: String
  }

  /// Walk every `AudioStreamRangedDescription` advertised by `stream`
  /// and translate it into our `PhysicalFormat`.
  static func availablePhysicalFormats(stream: AudioStreamID) -> [PhysicalFormat] {
    var addr = AudioObjectPropertyAddress(
      mSelector: kAudioStreamPropertyAvailablePhysicalFormats,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain
    )
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(stream, &addr, 0, nil, &size) == noErr,
      size > 0
    else { return [] }
    let count = Int(size) / MemoryLayout<AudioStreamRangedDescription>.size
    var ranged = [AudioStreamRangedDescription](
      repeating: AudioStreamRangedDescription(),
      count: count
    )
    guard AudioObjectGetPropertyData(stream, &addr, 0, nil, &size, &ranged) == noErr else {
      return []
    }

    return ranged.compactMap { entry -> PhysicalFormat? in
      let asbd = entry.mFormat
      guard asbd.mFormatID == kAudioFormatLinearPCM else { return nil }
      let formatString = formatStringFor(asbd: asbd)
      guard !formatString.isEmpty else { return nil }
      let rates = sampleRates(in: entry.mSampleRateRange, hint: asbd.mSampleRate)
      return PhysicalFormat(
        channels: Int(asbd.mChannelsPerFrame),
        samplerates: rates,
        formatString: formatString
      )
    }
  }

  /// Map an AudioStreamBasicDescription to a CamillaDSP CoreAudio sample
  /// format token. The token set matches Rust's `CoreAudioSampleFormat`
  /// enum (S16, S24, S32, F32) — exactly the formats the CoreAudio
  /// backend accepts. Anything else (e.g. 64-bit float, unsigned PCM)
  /// returns an empty string and is filtered out by the caller.
  static func formatStringFor(asbd: AudioStreamBasicDescription) -> String {
    let flags = asbd.mFormatFlags
    let isFloat = (flags & kAudioFormatFlagIsFloat) != 0
    let isSignedInt = (flags & kAudioFormatFlagIsSignedInteger) != 0
    let bits = asbd.mBitsPerChannel

    if isFloat, bits == 32 { return SampleFormat.f32.rawValue }
    if isSignedInt {
      switch bits {
      case 16: return SampleFormat.s16.rawValue
      case 24: return SampleFormat.s24.rawValue
      case 32: return SampleFormat.s32.rawValue
      default: return ""
      }
    }
    return ""
  }

  /// Pick standard sample rates that fall inside the device's range.
  /// Devices that advertise a single discrete rate report
  /// `mMinimum == mMaximum`; in that case we return the single value
  /// (rounded to `Int`).
  private static func sampleRates(in range: AudioValueRange, hint: Float64) -> [Int] {
    let lo = range.mMinimum
    let hi = range.mMaximum
    if lo == hi {
      return [Int(lo.rounded())]
    }
    var matched = standardRates.filter { Float64($0) >= lo && Float64($0) <= hi }
    // Always include the hint rate (the format's natural rate) if it
    // doesn't already line up with a standard rate — some pro devices
    // sit at oddball rates like 50.4 kHz.
    let hintInt = Int(hint.rounded())
    if hintInt > 0, !matched.contains(hintInt) {
      matched.append(hintInt)
      matched.sort()
    }
    return matched
  }
}
