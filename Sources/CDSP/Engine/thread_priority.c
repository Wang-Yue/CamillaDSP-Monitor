#include "thread_priority.h"

#include "Logging/app_logger.h"
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif
#include <pthread.h>
#include <stdio.h>

/// Bind the *calling* thread to a Mach time-constraint scheduling policy
/// tailored to the given audio buffer parameters.
///
/// This is the standard Darwin/macOS idiom for real-time audio threads.
///
/// - Parameters:
///   - name: A descriptive name of the thread (e.g. Capture, Playback,
///   Processing).
///   - buffer_frames: The buffer size in frames.
///   - sample_rate: The sample rate in Hz.
#ifdef __APPLE__
void set_realtime_thread_priority(const char* name, size_t buffer_frames,
                                  size_t sample_rate) {
  if (buffer_frames == 0 || sample_rate == 0) {
    logger_t logger = logger_create("dsp.threadpriority");
    logger_warn(&logger,
                "[%s] Invalid audio parameters for real-time priority: "
                "frames=%d, rate=%d",
                log_arg_string(name ? name : "unknown"),
                log_arg_int((int64_t)buffer_frames),
                log_arg_int((int64_t)sample_rate), log_arg_none());
    return;
  }

  mach_timebase_info_data_t tb_info;
  kern_return_t status = mach_timebase_info(&tb_info);
  if (status != KERN_SUCCESS) {
    logger_t logger = logger_create("dsp.threadpriority");
    logger_error(&logger, "[%s] Failed to retrieve Mach timebase info: %d",
                 log_arg_string(name ? name : "unknown"),
                 log_arg_int((int64_t)status), log_arg_none(), log_arg_none());
    return;
  }

  // Calculate nominal buffer period in nanoseconds.
  double period_ns =
      ((double)buffer_frames * 1000000000.0) / (double)sample_rate;

  // Allocate a computation budget (50% of the period) and constraint (100% of
  // the period).
  double computation_ns = period_ns * 0.5;
  double constraint_ns = period_ns;

  // Cap computation budget at 50ms per macOS limits.
  // Darwin prevents setting real-time quanta larger than 50ms to protect
  // the system from lockups if a real-time thread spins endlessly.
  double max_quantum_ns = 50000000.0;
  if (computation_ns > max_quantum_ns) {
    logger_t logger = logger_create("dsp.threadpriority");
    logger_info(
        &logger,
        "[%s] Thread computation budget capped at 50.0ms (%.1fms requested)",
        log_arg_string(name ? name : "unknown"),
        log_arg_double(computation_ns / 1000000.0), log_arg_none(),
        log_arg_none());
    computation_ns = max_quantum_ns;
  }

  // Convert nanoseconds to Mach absolute time units:
  // mach_units = nanoseconds * denom / numer.
  // This is required because macOS scheduling policies operate on CPU-specific
  // bus cycles / absolute time units rather than standard SI time.
  double numer = (double)tb_info.numer;
  double denom = (double)tb_info.denom;

  uint32_t period_mach = (uint32_t)((period_ns * denom) / numer);
  uint32_t computation_mach = (uint32_t)((computation_ns * denom) / numer);
  uint32_t constraint_mach = (uint32_t)((constraint_ns * denom) / numer);

  thread_time_constraint_policy_data_t policy = {
      .period = period_mach,
      .computation = computation_mach,
      .constraint = constraint_mach,
      .preemptible = 1};

  mach_msg_type_number_t count =
      sizeof(thread_time_constraint_policy_data_t) / sizeof(integer_t);
  thread_port_t thread = mach_thread_self();

  kern_return_t result = thread_policy_set(
      thread, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&policy, count);

  logger_t logger = logger_create("dsp.threadpriority");
  if (result == KERN_SUCCESS) {
    logger_info(&logger,
                "[%s] Thread promoted to real-time priority: period=%.1fms, "
                "computation=%.1fms, constraint=%.1fms",
                log_arg_string(name ? name : "unknown"),
                log_arg_double(period_ns / 1000000.0),
                log_arg_double(computation_ns / 1000000.0),
                log_arg_double(constraint_ns / 1000000.0));
  } else {
    logger_error(&logger, "[%s] Failed to set real-time thread policy: %d",
                 log_arg_string(name ? name : "unknown"),
                 log_arg_int((int64_t)result), log_arg_none(), log_arg_none());
  }
}
#elif defined(__linux__)
#include <sched.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

void set_realtime_thread_priority(const char* name, size_t buffer_frames,
                                  size_t sample_rate) {
  (void)buffer_frames;
  (void)sample_rate;
  pthread_t thread = pthread_self();
  struct sched_param param;
  int policy;
  logger_t logger = logger_create("dsp.threadpriority");

  // 1. Try native POSIX scheduling first.
  // This requires the process to have CAP_SYS_NICE capability or configured
  // rlimits (e.g. in /etc/security/limits.conf or via systemd LimitRTPRIO). If
  // successful, we transition the thread to SCHED_FIFO scheduling class.
  if (pthread_getschedparam(thread, &policy, &param) == 0) {
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    int res = pthread_setschedparam(thread, SCHED_FIFO, &param);
    if (res == 0) {
      logger_info(&logger,
                  "[%s] Thread promoted to Linux SCHED_FIFO real-time priority "
                  "via pthread_setschedparam",
                  log_arg_string(name ? name : "unknown"), log_arg_none(),
                  log_arg_none(), log_arg_none());
      return;
    }
  }

  // 2. Fall back to RealtimeKit (rtkit) via dbus-send.
  // rtkit is a system daemon that acts as an broker for unprivileged threads.
  // It allows threads to promote themselves to real-time SCHED_RESET_ON_FORK
  // status without needing root or CAP_SYS_NICE, via a D-Bus interface. We
  // execute dbus-send command directly since we do not link against dbus
  // library.
  pid_t tid = (pid_t)syscall(SYS_gettid);
  char cmd[512];
  // rtkit priority default is 10 (matching RT_PRIO_DEFAULT in
  // audio_thread_priority)
  snprintf(cmd, sizeof(cmd),
           "dbus-send --system --print-reply "
           "--dest=org.freedesktop.RealtimeKit1 /org/freedesktop/RealtimeKit1 "
           "org.freedesktop.RealtimeKit1.MakeThreadRealtime uint64:%llu "
           "uint32:%u >/dev/null 2>&1",
           (unsigned long long)tid, 10);
  int rtkit_res = system(cmd);
  if (rtkit_res == 0) {
    logger_info(&logger,
                "[%s] Thread promoted to Linux real-time priority via "
                "RealtimeKit (rtkit)",
                log_arg_string(name ? name : "unknown"), log_arg_none(),
                log_arg_none(), log_arg_none());
  } else {
    logger_warn(&logger,
                "[%s] Failed to promote thread to real-time priority (both "
                "pthread_setschedparam and RealtimeKit failed)",
                log_arg_string(name ? name : "unknown"), log_arg_none(),
                log_arg_none(), log_arg_none());
  }
}
#elif defined(_WIN32)
#include <windows.h>
void set_realtime_thread_priority(const char* name, size_t buffer_frames,
                                  size_t sample_rate) {
  (void)buffer_frames;
  (void)sample_rate;
  HANDLE thread = GetCurrentThread();
  BOOL success = SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);
  logger_t logger = logger_create("dsp.threadpriority");
  if (success) {
    logger_info(&logger,
                "[%s] Thread promoted to Windows THREAD_PRIORITY_TIME_CRITICAL",
                log_arg_string(name ? name : "unknown"), log_arg_none(),
                log_arg_none(), log_arg_none());
  } else {
    logger_warn(
        &logger, "[%s] Failed to set thread priority on Windows: err=%lu",
        log_arg_string(name ? name : "unknown"),
        log_arg_int((int64_t)GetLastError()), log_arg_none(), log_arg_none());
  }
}
#else
void set_realtime_thread_priority(const char* name, size_t buffer_frames,
                                  size_t sample_rate) {
  (void)name;
  (void)buffer_frames;
  (void)sample_rate;
}
#endif
