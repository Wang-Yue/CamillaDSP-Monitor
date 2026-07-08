#ifndef CLIB_ENGINE_THREAD_PRIORITY_H
#define CLIB_ENGINE_THREAD_PRIORITY_H

/**
 * @file thread_priority.h
 * @brief Helper for promoting threads to Mach real-time priority based on audio
 * parameters (buffer frames and sample rate).
 */

#include <stddef.h>

/**
 * @brief Binds the *calling* thread to a Mach time-constraint scheduling policy
 * tailored to the given audio buffer parameters.
 *
 * This is the standard Darwin/macOS idiom for real-time audio threads.
 *
 * @param name A descriptive name of the thread (e.g. "Capture", "Playback",
 * "Processing").
 * @param buffer_frames The buffer size in frames.
 * @param sample_rate The sample rate in Hz.
 */
void set_realtime_thread_priority(const char* name, size_t buffer_frames,
                                  size_t sample_rate);

#endif  // CLIB_ENGINE_THREAD_PRIORITY_H
