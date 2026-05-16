// Helper for promoting threads to Mach real-time priority
// based on audio parameters (buffer frames and sample rate).

import DSPLogging
import Darwin
import Foundation

private let logger = Logger(label: "dsp.threadpriority")

/// Bind the *calling* thread to a Mach time-constraint scheduling policy
/// tailored to the given audio buffer parameters.
///
/// This is the standard Darwin/macOS idiom for real-time audio threads.
///
/// - Parameters:
///   - name: A descriptive name of the thread (e.g. Capture, Playback, Processing).
///   - bufferFrames: The buffer size in frames.
///   - sampleRate: The sample rate in Hz.
internal func setRealtimeThreadPriority(name: StaticString, bufferFrames: Int, sampleRate: Int) {
  guard bufferFrames > 0, sampleRate > 0 else {
    logger.warning(
      "[%s] Invalid audio parameters for real-time priority: frames=%d, rate=%d",
      .staticString(name), .int(bufferFrames), .int(sampleRate))
    return
  }

  var tbInfo = mach_timebase_info_data_t()
  let status = mach_timebase_info(&tbInfo)
  guard status == KERN_SUCCESS else {
    logger.error(
      "[%s] Failed to retrieve Mach timebase info: %d", .staticString(name), .int(Int(status)))
    return
  }

  // Calculate nominal buffer period in nanoseconds.
  let periodNs = (Double(bufferFrames) * 1_000_000_000.0) / Double(sampleRate)

  // Allocate a computation budget (50% of the period) and constraint (100% of the period).
  var computationNs = periodNs * 0.5
  let constraintNs = periodNs

  // Cap computation budget at 50ms per macOS limits.
  let maxQuantumNs = 50_000_000.0
  if computationNs > maxQuantumNs {
    logger.info(
      "[%s] Thread computation budget capped at 50.0ms (%.1fms requested)",
      .staticString(name), .double(computationNs / 1_000_000.0))
    computationNs = maxQuantumNs
  }

  // Convert nanoseconds to Mach absolute time units:
  // mach_units = nanoseconds * denom / numer
  let numer = Double(tbInfo.numer)
  let denom = Double(tbInfo.denom)

  let periodMach = UInt32((periodNs * denom) / numer)
  let computationMach = UInt32((computationNs * denom) / numer)
  let constraintMach = UInt32((constraintNs * denom) / numer)

  var policy = thread_time_constraint_policy_data_t(
    period: periodMach,
    computation: computationMach,
    constraint: constraintMach,
    preemptible: 1
  )

  let count = mach_msg_type_number_t(
    MemoryLayout<thread_time_constraint_policy_data_t>.size / MemoryLayout<integer_t>.size
  )
  let thread = mach_thread_self()

  let result = withUnsafeMutablePointer(to: &policy) { ptr in
    ptr.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { intPtr in
      thread_policy_set(thread, UInt32(THREAD_TIME_CONSTRAINT_POLICY), intPtr, count)
    }
  }

  if result == KERN_SUCCESS {
    logger.info(
      "[%s] Thread promoted to real-time priority: period=%.1fms, computation=%.1fms, constraint=%.1fms",
      .staticString(name),
      .double(periodNs / 1_000_000.0),
      .double(computationNs / 1_000_000.0),
      .double(constraintNs / 1_000_000.0)
    )
  } else {
    logger.error(
      "[%s] Failed to set real-time thread policy: %d", .staticString(name), .int(Int(result)))
  }
}
