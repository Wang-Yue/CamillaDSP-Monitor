// Shared CoreAudio HAL helpers used by both `CoreAudioBackend` (the
// capture/playback runtime) and `CoreAudioCapabilities` (the device
// description discovery). Keeps the boilerplate around
// `AudioObjectGetPropertyData` and friends in one place so the two
// backends don't carry near-identical copies of every enumeration helper.

import AudioToolbox
import CoreAudio
import Foundation
import Synchronization

/// Direction marker for HAL device queries. The `kAudioDevicePropertyScopeInput`
/// and `kAudioDevicePropertyScopeOutput` constants are aliases of
/// `kAudioObjectPropertyScopeInput`/`Output`, so the same value works
/// for stream-config queries and stream-list queries.
enum CoreAudioScope {
  case input, output

  var streamScope: AudioObjectPropertyScope {
    switch self {
    case .input: return kAudioDevicePropertyScopeInput
    case .output: return kAudioDevicePropertyScopeOutput
    }
  }

  var defaultDeviceSelector: AudioObjectPropertySelector {
    switch self {
    case .input: return kAudioHardwarePropertyDefaultInputDevice
    case .output: return kAudioHardwarePropertyDefaultOutputDevice
    }
  }
}

/// Pure-helper namespace for enumerating and identifying CoreAudio HAL
/// devices. None of the methods here mutate state; they're safe to call
/// concurrently.
enum CoreAudioDevice {

  // MARK: Enumeration

  /// Every HAL device on the system, regardless of stream direction.
  static func allIDs() -> [AudioDeviceID] {
    var addr = AudioObjectPropertyAddress(
      mSelector: kAudioHardwarePropertyDevices,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain
    )
    var size: UInt32 = 0
    guard
      AudioObjectGetPropertyDataSize(
        AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size
      ) == noErr, size > 0
    else { return [] }
    let count = Int(size) / MemoryLayout<AudioDeviceID>.size
    var ids = [AudioDeviceID](repeating: 0, count: count)
    guard
      AudioObjectGetPropertyData(
        AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &ids
      ) == noErr
    else { return [] }
    return ids
  }

  /// User-facing name of a device, or `nil` if the lookup fails.
  static func name(of deviceID: AudioDeviceID) -> String? {
    var addr = AudioObjectPropertyAddress(
      mSelector: kAudioObjectPropertyName,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain
    )
    var name: Unmanaged<CFString>?
    var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    guard AudioObjectGetPropertyData(deviceID, &addr, 0, nil, &size, &name) == noErr else {
      return nil
    }
    return name?.takeRetainedValue() as String?
  }

  /// True if the device exposes any streams in the given direction.
  static func hasStream(deviceID: AudioDeviceID, scope: CoreAudioScope) -> Bool {
    var addr = AudioObjectPropertyAddress(
      mSelector: kAudioDevicePropertyStreams,
      mScope: scope.streamScope,
      mElement: kAudioObjectPropertyElementMain
    )
    var size: UInt32 = 0
    let status = AudioObjectGetPropertyDataSize(deviceID, &addr, 0, nil, &size)
    return status == noErr && size >= UInt32(MemoryLayout<AudioStreamID>.size)
  }

  /// HAL stream IDs for the given device + direction.
  static func streams(of deviceID: AudioDeviceID, scope: CoreAudioScope) -> [AudioStreamID] {
    var addr = AudioObjectPropertyAddress(
      mSelector: kAudioDevicePropertyStreams,
      mScope: scope.streamScope,
      mElement: kAudioObjectPropertyElementMain
    )
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(deviceID, &addr, 0, nil, &size) == noErr,
      size > 0
    else { return [] }
    let count = Int(size) / MemoryLayout<AudioStreamID>.size
    var ids = [AudioStreamID](repeating: 0, count: count)
    guard AudioObjectGetPropertyData(deviceID, &addr, 0, nil, &size, &ids) == noErr else {
      return []
    }
    return ids
  }

  /// Devices that have at least one stream in the requested direction,
  /// each paired with its user-facing name. Devices that fail the
  /// stream-config check (e.g. an output-only device queried in
  /// `.input` scope) are filtered out.
  static func devices(scope: CoreAudioScope) -> [(id: AudioDeviceID, name: String)] {
    allIDs()
      .filter { hasStream(deviceID: $0, scope: scope) }
      .map { ($0, name(of: $0) ?? "") }
  }

  // MARK: Lookup

  /// HAL ID of the system-default device for the given direction.
  static func defaultDeviceID(scope: CoreAudioScope) -> AudioDeviceID? {
    var addr = AudioObjectPropertyAddress(
      mSelector: scope.defaultDeviceSelector,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain
    )
    var id: AudioDeviceID = 0
    var size = UInt32(MemoryLayout<AudioDeviceID>.size)
    guard
      AudioObjectGetPropertyData(
        AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &id
      ) == noErr
    else { return nil }
    return id
  }

  /// HAL ID of a named device, or the system default when `name` is
  /// `nil`. Returns `nil` if the named device can't be found.
  static func deviceID(forName name: String?, scope: CoreAudioScope) -> AudioDeviceID? {
    if let name {
      return devices(scope: scope).first(where: { $0.name == name })?.id
    }
    return defaultDeviceID(scope: scope)
  }

  // MARK: Sample-rate control

  /// Push the device's nominal sample rate, then poll until the
  /// change has been committed. CoreAudio applies the change
  /// asynchronously on a HAL thread; if we proceed straight to
  /// `AudioUnitInitialize` the AudioUnit can latch the *old* rate
  /// and silently sample-rate-convert from then on. Returns `true`
  /// only when both the set call succeeded *and* the device's
  /// reported rate matches `rate` within `~0.5 Hz` after the poll.
  ///
  /// Devices that don't support the requested rate return a
  /// non-zero status from the set call; we surface that as `false`
  /// without polling.
  @discardableResult
  static func setNominalSampleRate(_ deviceID: AudioDeviceID, _ rate: Double) -> Bool {
    var addr = AudioObjectPropertyAddress(
      mSelector: kAudioDevicePropertyNominalSampleRate,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain
    )
    var value = rate
    let status = AudioObjectSetPropertyData(
      deviceID, &addr, 0, nil,
      UInt32(MemoryLayout<Float64>.size), &value
    )
    guard status == noErr else { return false }
    // Poll up to ~250 ms for the device to actually flip rates.
    // 5 ms granularity is fast enough that callers don't notice
    // the wait while still letting the HAL settle.
    let deadline = Date().addingTimeInterval(0.25)
    while Date() < deadline {
      if let current = nominalSampleRate(deviceID), abs(current - rate) < 0.5 {
        return true
      }
      Thread.sleep(forTimeInterval: 0.005)
    }
    return false
  }

  /// Read the device's current nominal sample rate. Used to verify
  /// that `setNominalSampleRate` actually took effect — CoreAudio
  /// applies the change asynchronously, so callers should poll this
  /// for a short window before falling back.
  static func nominalSampleRate(_ deviceID: AudioDeviceID) -> Double? {
    var addr = AudioObjectPropertyAddress(
      mSelector: kAudioDevicePropertyNominalSampleRate,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain
    )
    var rate: Float64 = 0
    var size = UInt32(MemoryLayout<Float64>.size)
    let status = AudioObjectGetPropertyData(deviceID, &addr, 0, nil, &size, &rate)
    return status == noErr ? rate : nil
  }

  // MARK: Stream-format builder

  // MARK: Clock-source / pitch control (BlackHole 0.5.0+)

  /// Enumerate clock sources for `deviceID`. Returns the parallel
  /// `(name, id)` arrays in declaration order. Used by
  /// `selectAdjustableClockSource` to find the magic
  /// `"Internal Adjustable"` source that BlackHole 0.5.0+ exposes
  /// for fine-grained pitch control.
  static func clockSourceNamesAndIDs(_ deviceID: AudioDeviceID)
    -> (names: [String], ids: [UInt32])
  {
    var addr = AudioObjectPropertyAddress(
      mSelector: kAudioDevicePropertyClockSources,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain
    )
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(deviceID, &addr, 0, nil, &size) == noErr,
      size > 0
    else { return ([], []) }
    let count = Int(size) / MemoryLayout<UInt32>.size
    var ids = [UInt32](repeating: 0, count: count)
    guard AudioObjectGetPropertyData(deviceID, &addr, 0, nil, &size, &ids) == noErr else {
      return ([], [])
    }
    var names: [String] = []
    for id in ids {
      var nameAddr = AudioObjectPropertyAddress(
        mSelector: kAudioDevicePropertyClockSourceNameForIDCFString,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
      )
      var sourceID = id
      var nameRef: Unmanaged<CFString>?
      // The `AudioValueTranslation` struct stores raw pointers
      // to `sourceID` / `nameRef`; both must outlive the call.
      // `withUnsafeMutablePointer` keeps the storage live for
      // the duration of the closure.
      let resolvedName: String? = withUnsafeMutablePointer(to: &sourceID) { inPtr in
        withUnsafeMutablePointer(to: &nameRef) { outPtr in
          var translation = AudioValueTranslation(
            mInputData: UnsafeMutableRawPointer(inPtr),
            mInputDataSize: UInt32(MemoryLayout<UInt32>.size),
            mOutputData: UnsafeMutableRawPointer(outPtr),
            mOutputDataSize: UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
          )
          var translationSize = UInt32(MemoryLayout<AudioValueTranslation>.size)
          let status = AudioObjectGetPropertyData(
            deviceID, &nameAddr, 0, nil, &translationSize, &translation
          )
          if status == noErr, let cf = outPtr.pointee?.takeRetainedValue() {
            return cf as String
          }
          return nil
        }
      }
      names.append(resolvedName ?? "")
    }
    return (names, ids)
  }

  /// Set the device's active clock source by ID. Returns `true` on
  /// success.
  @discardableResult
  static func setClockSourceID(_ deviceID: AudioDeviceID, _ sourceID: UInt32) -> Bool {
    var addr = AudioObjectPropertyAddress(
      mSelector: kAudioDevicePropertyClockSource,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain
    )
    var value = sourceID
    let status = AudioObjectSetPropertyData(
      deviceID, &addr, 0, nil,
      UInt32(MemoryLayout<UInt32>.size), &value
    )
    return status == noErr
  }

  /// If `deviceID` advertises an "Internal Adjustable" clock source
  /// (BlackHole 0.5.0+), select it as the active source and return
  /// `true`. Returns `false` for devices that don't support pitch
  /// tuning. Mirrors `configure_pitch_control` in
  /// `camilladsp/src/coreaudio_backend/device.rs`.
  static func selectAdjustableClockSource(_ deviceID: AudioDeviceID) -> Bool {
    let (names, ids) = clockSourceNamesAndIDs(deviceID)
    guard !names.isEmpty,
      let idx = names.firstIndex(of: "Internal Adjustable")
    else { return false }
    return setClockSourceID(deviceID, ids[idx])
  }

  /// Apply a clock-pitch correction to the capture device by
  /// writing `kAudioDevicePropertyStereoPan`. Upstream maps
  /// `pitch ∈ [0.99, 1.01]` to `pan ∈ [0, 1]` with the formula
  /// `pan = (pitch - 1.0) * 50.0 + 0.5`, clamped to the valid
  /// range. Direct port of `set_pitch` in
  /// `camilladsp/src/coreaudio_backend/device.rs`.
  static func setDevicePitch(_ deviceID: AudioDeviceID, pitch: Double) {
    var addr = AudioObjectPropertyAddress(
      mSelector: kAudioDevicePropertyStereoPan,
      mScope: kAudioObjectPropertyScopeOutput,
      mElement: kAudioObjectPropertyElementMain
    )
    var pan: Float = Float((pitch - 1.0) * 50.0 + 0.5)
    if pan < 0 { pan = 0 }
    if pan > 1 { pan = 1 }
    _ = AudioObjectSetPropertyData(
      deviceID, &addr, 0, nil,
      UInt32(MemoryLayout<Float>.size), &pan
    )
  }

  /// Returns true if the device exposes the nominal-sample-rate
  /// property — needed before installing a `RateChangeWatcher` so we
  /// don't churn HAL listener registrations on devices that can't
  /// publish rate changes anyway.
  static func hasNominalSampleRateProperty(_ deviceID: AudioDeviceID) -> Bool {
    var addr = AudioObjectPropertyAddress(
      mSelector: kAudioDevicePropertyNominalSampleRate,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain
    )
    return AudioObjectHasProperty(deviceID, &addr)
  }

  /// Standard 32-bit linear-PCM ASBD used by both backends. Pass
  /// `interleaved: false` for the non-interleaved layout the engine
  /// prefers (one HAL buffer per channel); `true` for the classic
  /// interleaved fallback (one buffer with all channels packed).
  static func float32StreamFormat(
    sampleRate: Double,
    channels: Int,
    interleaved: Bool
  ) -> AudioStreamBasicDescription {
    let bytesPerFrame = UInt32(interleaved ? 4 * channels : 4)
    var flags: AudioFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
    if !interleaved { flags |= kAudioFormatFlagIsNonInterleaved }
    return AudioStreamBasicDescription(
      mSampleRate: sampleRate,
      mFormatID: kAudioFormatLinearPCM,
      mFormatFlags: flags,
      mBytesPerPacket: bytesPerFrame,
      mFramesPerPacket: 1,
      mBytesPerFrame: bytesPerFrame,
      mChannelsPerFrame: UInt32(channels),
      mBitsPerChannel: 32,
      mReserved: 0
    )
  }
}

/// Watches a CoreAudio device's `kAudioDevicePropertyNominalSampleRate`
/// and reports any change away from the rate the engine asked for.
///
/// Mirrors the role of `RateListener` in the Rust upstream
/// (`coreaudio_backend/device.rs`), which uses the equivalent
/// `coreaudio-rs` listener registration. The processing thread polls
/// `pendingRateChange` once per chunk; on a real change it stops the
/// engine with `.captureFormatChange(rate)` / `.playbackFormatChange(rate)`
/// so the host can rebuild at the new rate.
///
/// Lifetime: created by `CoreAudioCapture.open()` /
/// `CoreAudioPlayback.open()` *after* `setNominalSampleRate` has been
/// applied (so the watcher's expected rate is the one we just pushed).
/// `dispose()` removes the HAL listener and must run before the owner
/// is deallocated — `deinit` calls it as a backstop.
final class RateChangeWatcher: Sendable {
  private let deviceID: AudioDeviceID
  private let expectedRate: Double
  /// Latest observed nominal rate, encoded as Double bit-pattern.
  /// `0` is the "no change observed yet" sentinel — `0.0.bitPattern`
  /// is also `0`, but a sample rate of 0 Hz is not a meaningful state
  /// to surface to the engine.
  private let latestRateBits = Atomic<UInt64>(0)
  private let registered = Atomic<Bool>(false)

  init(deviceID: AudioDeviceID, expectedRate: Double) {
    self.deviceID = deviceID
    self.expectedRate = expectedRate
    var addr = AudioObjectPropertyAddress(
      mSelector: kAudioDevicePropertyNominalSampleRate,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain
    )
    let opaque = Unmanaged.passUnretained(self).toOpaque()
    let status = AudioObjectAddPropertyListener(
      deviceID, &addr, rateChangeListenerCallback, opaque
    )
    registered.store(status == noErr, ordering: .releasing)
  }

  deinit { dispose() }

  /// Latest rate observed via the HAL listener, or `nil` if no change
  /// has fired yet or the observed rate matches the rate we asked
  /// for. The `≈ 0.5 Hz` tolerance filters out the listener firing
  /// for our own `setNominalSampleRate` write.
  var pendingRateChange: Double? {
    let bits = latestRateBits.load(ordering: .acquiring)
    guard bits != 0 else { return nil }
    let rate = Double(bitPattern: bits)
    if abs(rate - expectedRate) < 0.5 { return nil }
    return rate
  }

  func dispose() {
    guard registered.load(ordering: .acquiring) else { return }
    var addr = AudioObjectPropertyAddress(
      mSelector: kAudioDevicePropertyNominalSampleRate,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain
    )
    let opaque = Unmanaged.passUnretained(self).toOpaque()
    AudioObjectRemovePropertyListener(
      deviceID, &addr, rateChangeListenerCallback, opaque
    )
    registered.store(false, ordering: .releasing)
  }

  fileprivate func record(_ rate: Double) {
    // Sample rate of 0 would alias the "not observed" sentinel,
    // which can't happen on a real device but is worth excluding
    // explicitly so a malformed report can't suppress the next
    // legitimate change.
    guard rate > 0 else { return }
    latestRateBits.store(rate.bitPattern, ordering: .releasing)
  }
}

/// HAL listener callback. Runs on a CoreAudio dispatch thread, never on
/// the render thread, so the `AudioObjectGetPropertyData` query here
/// is safe — but we still keep it to a single read + atomic store so
/// the listener returns promptly.
private let rateChangeListenerCallback: AudioObjectPropertyListenerProc = {
  (deviceID, _, _, refCon) -> OSStatus in
  guard let refCon else { return noErr }
  let watcher = Unmanaged<RateChangeWatcher>.fromOpaque(refCon).takeUnretainedValue()
  var addr = AudioObjectPropertyAddress(
    mSelector: kAudioDevicePropertyNominalSampleRate,
    mScope: kAudioObjectPropertyScopeGlobal,
    mElement: kAudioObjectPropertyElementMain
  )
  var rate: Float64 = 0
  var size = UInt32(MemoryLayout<Float64>.size)
  let status = AudioObjectGetPropertyData(deviceID, &addr, 0, nil, &size, &rate)
  if status == noErr {
    watcher.record(rate)
  }
  return noErr
}
