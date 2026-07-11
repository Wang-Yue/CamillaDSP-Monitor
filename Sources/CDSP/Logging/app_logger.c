// Lock-free, allocation-free high performance logger for real-time audio
// threads
#include "Logging/app_logger.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <dispatch/dispatch.h>
typedef dispatch_semaphore_t app_logger_sem_t;
/**
 * @brief Initializes a semaphore wrapper.
 * @param sem Pointer to the semaphore wrapper.
 */
static inline void __attribute__((unused)) app_logger_sem_init(
    app_logger_sem_t* sem) {
  *sem = dispatch_semaphore_create(0);
}
/**
 * @brief Destroys a semaphore wrapper.
 * @param sem Pointer to the semaphore wrapper.
 */
static inline void __attribute__((unused)) app_logger_sem_destroy(
    app_logger_sem_t* sem) {
  dispatch_release(*sem);
}
/**
 * @brief Signals a semaphore wrapper.
 * @param sem Pointer to the semaphore wrapper.
 */
static inline void __attribute__((unused)) app_logger_sem_signal(
    app_logger_sem_t* sem) {
  dispatch_semaphore_signal(*sem);
}
/**
 * @brief Waits on a semaphore wrapper.
 * @param sem Pointer to the semaphore wrapper.
 */
static inline void __attribute__((unused)) app_logger_sem_wait(
    app_logger_sem_t* sem) {
  dispatch_semaphore_wait(*sem, DISPATCH_TIME_FOREVER);
}
#else
#include <semaphore.h>
typedef sem_t app_logger_sem_t;
/**
 * @brief Initializes a semaphore wrapper.
 * @param sem Pointer to the semaphore wrapper.
 */
static inline void __attribute__((unused)) app_logger_sem_init(
    app_logger_sem_t* sem) {
  sem_init(sem, 0, 0);
}
/**
 * @brief Destroys a semaphore wrapper.
 * @param sem Pointer to the semaphore wrapper.
 */
static inline void __attribute__((unused)) app_logger_sem_destroy(
    app_logger_sem_t* sem) {
  sem_destroy(sem);
}
/**
 * @brief Signals a semaphore wrapper.
 * @param sem Pointer to the semaphore wrapper.
 */
static inline void __attribute__((unused)) app_logger_sem_signal(
    app_logger_sem_t* sem) {
  sem_post(sem);
}
/**
 * @brief Waits on a semaphore wrapper.
 * @param sem Pointer to the semaphore wrapper.
 */
static inline void __attribute__((unused)) app_logger_sem_wait(
    app_logger_sem_t* sem) {
  sem_wait(sem);
}
#endif

struct app_logger_s {
  log_record_t* storage;
  _Atomic uint64_t* sequences;
  size_t capacity;
  size_t mask;
  _Atomic uint64_t write_index;
  _Atomic uint64_t read_index;
  app_logger_sem_t semaphore;
  _Atomic bool should_exit;
  _Atomic bool is_started;
  pthread_t worker_thread;
  pthread_mutex_t worker_mutex;
};

/// Process-wide log-level gate. Stored as an atomic uint8_t so the
/// real-time audio path can read it without locks.
static _Atomic uint8_t g_current_log_level = LOG_LEVEL_INFO;
static app_logger_t* g_shared_logger = NULL;
static pthread_once_t g_logger_once = PTHREAD_ONCE_INIT;

log_level_t app_logger_get_level(void) {
  return (log_level_t)atomic_load_explicit(&g_current_log_level,
                                           memory_order_acquire);
}

void app_logger_set_level(log_level_t level) {
  atomic_store_explicit(&g_current_log_level, (uint8_t)level,
                        memory_order_release);
}

/**
 * @brief Safely formats a log message with up to 4 arguments.
 *
 * This function parses a format string similar to printf, but restricts parsing
 * to a maximum of 4 pre-packaged arguments (log_argument_t). It avoids standard
 * stdarg/varargs to allow safe deferral of string formatting to the background
 * worker thread. It supports integer, double, and string formats.
 *
 * @param out Buffer to write the formatted string to.
 * @param out_cap Capacity of the output buffer.
 * @param msg The printf-like format string.
 * @param args Array of exactly 4 log arguments.
 */
static void format_log_message(char* out, size_t out_cap, const char* msg,
                               const log_argument_t args[4]) {
  if (!out || out_cap == 0) return;
  if (!msg) {
    out[0] = '\0';
    return;
  }

  size_t out_len = 0;
  int arg_idx = 0;
  const char* p = msg;

  while (*p != '\0') {
    if (*p != '%') {
      if (out_len + 1 < out_cap) {
        out[out_len++] = *p;
        out[out_len] = '\0';
      }
      p++;
      continue;
    }

    // *p is '%'
    if (*(p + 1) == '%') {
      if (out_len + 1 < out_cap) {
        out[out_len++] = '%';
        out[out_len] = '\0';
      }
      p += 2;
      continue;
    }

    if (*(p + 1) == '\0') {
      if (out_len + 1 < out_cap) {
        out[out_len++] = '%';
        out[out_len] = '\0';
      }
      p++;
      break;
    }

    // We have a potential format specifier starting at p
    const char* spec_start = p;
    const char* q = p + 1;
    while (*q && strchr("-+ #0'0123456789.hljztLq", *q)) {
      q++;
    }

    char conv = *q;
    if (!conv || !strchr("diouxXfFeEgGaAcsp", conv)) {
      // Not a valid/supported conversion character, just emit '%' and advance
      if (out_len + 1 < out_cap) {
        out[out_len++] = '%';
        out[out_len] = '\0';
      }
      p++;
      continue;
    }

    // We found a valid specifier from spec_start to q (inclusive)
    // Check if we have an argument available
    if (arg_idx >= 4 || args[arg_idx].type == LOG_ARG_NONE) {
      // No argument left; copy the specifier literally
      for (const char* s = spec_start; s <= q; s++) {
        if (out_len + 1 < out_cap) {
          out[out_len++] = *s;
          out[out_len] = '\0';
        }
      }
      p = q + 1;
      continue;
    }

    log_argument_t arg = args[arg_idx++];
    char tmp[32768];
    tmp[0] = '\0';

    if (strchr("diouxXcp", conv)) {
      if (arg.type == LOG_ARG_INT) {
        if (conv == 'c') {
          snprintf(tmp, sizeof(tmp), "%c", (int)arg.val.i);
        } else if (conv == 'p') {
          snprintf(tmp, sizeof(tmp), "%p", (void*)(uintptr_t)arg.val.i);
        } else {
          // Build format specifier with "ll" length modifier
          char fmt[64];
          size_t flen = 0;
          for (const char* s = spec_start; s < q && flen < sizeof(fmt) - 5;
               s++) {
            if (!strchr("hljztqL", *s)) {
              fmt[flen++] = *s;
            }
          }
          fmt[flen++] = 'l';
          fmt[flen++] = 'l';
          fmt[flen++] = conv;
          fmt[flen] = '\0';

          if (strchr("uxXo", conv)) {
            snprintf(tmp, sizeof(tmp), fmt, (unsigned long long)arg.val.i);
          } else {
            snprintf(tmp, sizeof(tmp), fmt, (long long)arg.val.i);
          }
        }
      } else if (arg.type == LOG_ARG_DOUBLE) {
        snprintf(tmp, sizeof(tmp), "%.6f", arg.val.d);
      } else if (arg.type == LOG_ARG_STRING) {
        snprintf(tmp, sizeof(tmp), "%s", arg.val.s);
      }
    } else if (strchr("fFeEgGaA", conv)) {
      if (arg.type == LOG_ARG_DOUBLE) {
        char fmt[64];
        size_t flen = 0;
        for (const char* s = spec_start; s < q && flen < sizeof(fmt) - 3; s++) {
          if (!strchr("lL", *s)) {
            fmt[flen++] = *s;
          }
        }
        fmt[flen++] = conv;
        fmt[flen] = '\0';
        snprintf(tmp, sizeof(tmp), fmt, arg.val.d);
      } else if (arg.type == LOG_ARG_INT) {
        snprintf(tmp, sizeof(tmp), "%lld", (long long)arg.val.i);
      } else if (arg.type == LOG_ARG_STRING) {
        snprintf(tmp, sizeof(tmp), "%s", arg.val.s);
      }
    } else if (conv == 's') {
      if (arg.type == LOG_ARG_STRING) {
        char fmt[64];
        size_t flen = 0;
        for (const char* s = spec_start; s < q && flen < sizeof(fmt) - 3; s++) {
          if (*s != 'l') {
            fmt[flen++] = *s;
          }
        }
        fmt[flen++] = conv;
        fmt[flen] = '\0';
        snprintf(tmp, sizeof(tmp), fmt, arg.val.s);
      } else if (arg.type == LOG_ARG_INT) {
        snprintf(tmp, sizeof(tmp), "%lld", (long long)arg.val.i);
      } else if (arg.type == LOG_ARG_DOUBLE) {
        snprintf(tmp, sizeof(tmp), "%.6f", arg.val.d);
      }
    }

    // Append tmp to out
    for (const char* s = tmp; *s != '\0'; s++) {
      if (out_len + 1 < out_cap) {
        out[out_len++] = *s;
        out[out_len] = '\0';
      }
    }

    p = q + 1;
  }

  // Append any unconsumed arguments
  for (; arg_idx < 4; arg_idx++) {
    if (args[arg_idx].type == LOG_ARG_NONE) break;
    char tmp[32768];
    tmp[0] = '\0';
    switch (args[arg_idx].type) {
      case LOG_ARG_NONE:
        break;
      case LOG_ARG_INT:
        snprintf(tmp, sizeof(tmp), " %lld", (long long)args[arg_idx].val.i);
        break;
      case LOG_ARG_DOUBLE:
        snprintf(tmp, sizeof(tmp), " %.6f", args[arg_idx].val.d);
        break;
      case LOG_ARG_STRING:
        snprintf(tmp, sizeof(tmp), " %s", args[arg_idx].val.s);
        break;
    }
    for (const char* s = tmp; *s != '\0'; s++) {
      if (out_len + 1 < out_cap) {
        out[out_len++] = *s;
        out[out_len] = '\0';
      }
    }
  }
}

/**
 * @brief Background worker thread that drains and prints log records.
 *
 * The worker blocks on a semaphore until logs are available. It then performs
 * a lock-free read from the ring buffer using atomic index updates.
 *
 * @param arg Pointer to the app_logger_t instance.
 * @return NULL.
 */
static void* worker_thread_func(void* arg) {
  app_logger_t* logger = (app_logger_t*)arg;
  while (!atomic_load_explicit(&logger->should_exit, memory_order_acquire)) {
    app_logger_sem_wait(&logger->semaphore);
    if (atomic_load_explicit(&logger->should_exit, memory_order_acquire)) {
      // Drain remaining records before exiting
    }

    while (true) {
      // Load current read index. Relaxed is sufficient because the sequence
      // checks below will establish the necessary happens-before relationships.
      uint64_t r =
          atomic_load_explicit(&logger->read_index, memory_order_relaxed);
      size_t slot = (size_t)(r & logger->mask);
      // Load the sequence number for the target slot with acquire semantics to
      // ensure we see the record written by app_logger_log.
      uint64_t seq =
          atomic_load_explicit(&logger->sequences[slot], memory_order_acquire);
      int64_t diff = (int64_t)seq - (int64_t)(r + 1);

      if (diff == 0) {
        // Safe to read: slot has been written and not yet processed.
        log_record_t rec = logger->storage[slot];
        // Release the slot back to the producer threads by updating its sequence.
        atomic_store_explicit(&logger->sequences[slot], r + logger->capacity,
                              memory_order_release);
        // Advance read index.
        atomic_store_explicit(&logger->read_index, r + 1, memory_order_relaxed);

      const char* lvl_str = "INFO";
      switch (rec.level) {
        case LOG_LEVEL_OFF:
          lvl_str = "OFF";
          break;
        case LOG_LEVEL_ERROR:
          lvl_str = "ERROR";
          break;
        case LOG_LEVEL_WARN:
          lvl_str = "WARN";
          break;
        case LOG_LEVEL_INFO:
          lvl_str = "INFO";
          break;
        case LOG_LEVEL_DEBUG:
          lvl_str = "DEBUG";
          break;
        case LOG_LEVEL_TRACE:
          lvl_str = "TRACE";
          break;
      }
      char formatted_msg[32768];
      log_argument_t args[4] = {rec.arg1, rec.arg2, rec.arg3, rec.arg4};
      format_log_message(formatted_msg, sizeof(formatted_msg), rec.message,
                         args);
      printf("[%s] %s: %s\n", lvl_str, rec.label ? rec.label : "",
             formatted_msg);
      fflush(stdout);
    } else {
      break;
    }
  }
  }
  return NULL;
}

static void free_logger_internal(app_logger_t* logger) {
  if (!logger) return;
  free(logger->storage);
  free(logger->sequences);
  free(logger);
}

/**
 * @brief Initializes the singleton logger instance.
 *
 * Called via pthread_once to ensure thread safety.
 */
static void init_shared_logger(void) {
  // Intentionally empty/default-init to guarantee safe singleton instance
  // publication before thread activation.
  g_shared_logger = (app_logger_t*)calloc(1, sizeof(app_logger_t));
  if (!g_shared_logger) return;
  g_shared_logger->capacity = 512;
  g_shared_logger->mask = 511;
  g_shared_logger->storage =
      (log_record_t*)calloc(g_shared_logger->capacity, sizeof(log_record_t));
  g_shared_logger->sequences =
      (_Atomic uint64_t*)calloc(g_shared_logger->capacity, sizeof(_Atomic uint64_t));
  if (!g_shared_logger->storage || !g_shared_logger->sequences) {
    free_logger_internal(g_shared_logger);
    g_shared_logger = NULL;
    return;
  }
  for (size_t i = 0; i < g_shared_logger->capacity; i++) {
    atomic_init(&g_shared_logger->sequences[i], (uint64_t)i);
  }
  atomic_init(&g_shared_logger->write_index, 0);
  atomic_init(&g_shared_logger->read_index, 0);
  atomic_init(&g_shared_logger->should_exit, false);
  atomic_init(&g_shared_logger->is_started, false);
  app_logger_sem_init(&g_shared_logger->semaphore);
  pthread_mutex_init(&g_shared_logger->worker_mutex, NULL);
}

app_logger_t* app_logger_get_shared(void) {
  pthread_once(&g_logger_once, init_shared_logger);
  return g_shared_logger;
}

void app_logger_log(app_logger_t* logger, log_level_t level, const char* label,
                    const char* message, log_argument_t arg1,
                    log_argument_t arg2, log_argument_t arg3,
                    log_argument_t arg4) {
  if (!logger || level > app_logger_get_level()) return;
  // Lazily start the background worker thread when the first log occurs.
  // Use compare-and-swap to ensure only one thread starts the worker.
  bool expected = false;
  if (atomic_compare_exchange_strong_explicit(&logger->is_started, &expected,
                                              true, memory_order_acq_rel,
                                              memory_order_acquire)) {
    pthread_mutex_lock(&logger->worker_mutex);
    pthread_create(&logger->worker_thread, NULL, worker_thread_func, logger);
    pthread_mutex_unlock(&logger->worker_mutex);
  }

  // Claim a slot in the ring buffer.
  uint64_t w = atomic_load_explicit(&logger->write_index, memory_order_relaxed);
  size_t slot;
  while (true) {
    slot = (size_t)(w & logger->mask);
    // Load sequence number with acquire to sync with slot release in consumer thread.
    uint64_t seq =
        atomic_load_explicit(&logger->sequences[slot], memory_order_acquire);
    int64_t diff = (int64_t)seq - (int64_t)w;
    if (diff == 0) {
      // Slot is available. Claim the write slot atomically.
      if (atomic_compare_exchange_weak_explicit(&logger->write_index, &w, w + 1,
                                                memory_order_relaxed,
                                                memory_order_relaxed)) {
        break;
      }
    } else if (diff < 0) {
      // Slot is not yet processed by consumer (queue is full). Drop log (non-blocking).
      return;
    } else {
      // Slot is in progress or write_index was advanced. Reload and try again.
      w = atomic_load_explicit(&logger->write_index, memory_order_relaxed);
    }
  }

  // Populate slot. Note: if string args are pointers to temporary stack,
  // this can cause issues. User of the logger should pass static or
  // heap-allocated strings, or strings that survive the background log
  // processing.
  logger->storage[slot].level = level;
  logger->storage[slot].label = label;
  logger->storage[slot].message = message;
  logger->storage[slot].arg1 = arg1;
  logger->storage[slot].arg2 = arg2;
  logger->storage[slot].arg3 = arg3;
  logger->storage[slot].arg4 = arg4;

  // Publish the written slot to the worker thread.
  atomic_store_explicit(&logger->sequences[slot], w + 1, memory_order_release);
  app_logger_sem_signal(&logger->semaphore);
}

void app_logger_flush_and_stop(app_logger_t* logger) {
  if (!logger) return;
  if (atomic_load_explicit(&logger->is_started, memory_order_acquire)) {
    atomic_store_explicit(&logger->should_exit, true, memory_order_release);
    app_logger_sem_signal(&logger->semaphore);
    pthread_join(logger->worker_thread, NULL);
    atomic_store_explicit(&logger->is_started, false, memory_order_release);
    atomic_store_explicit(&logger->should_exit, false, memory_order_release);
  }
}
