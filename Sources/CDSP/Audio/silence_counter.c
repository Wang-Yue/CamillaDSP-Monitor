// SilenceCounter — counts consecutive silent chunks against a dB threshold.
#include "Audio/silence_counter.h"

#include <math.h>
#include <stdlib.h>

struct silence_counter {
  size_t limit_chunks;
  double threshold_db;
  size_t silent_chunks;
};

silence_counter_t* silence_counter_create(double threshold_db,
                                          double timeout_seconds,
                                          size_t samplerate, size_t chunksize) {
  silence_counter_t* counter =
      (silence_counter_t*)malloc(sizeof(silence_counter_t));
  if (!counter) return NULL;
  silence_counter_init(counter, threshold_db, timeout_seconds, samplerate,
                       chunksize);
  return counter;
}

void silence_counter_free(silence_counter_t* counter) {
  if (counter) free(counter);
}

size_t silence_counter_get_limit_chunks(const silence_counter_t* counter) {
  return counter ? counter->limit_chunks : 0;
}

size_t silence_counter_get_silent_chunks(const silence_counter_t* counter) {
  return counter ? counter->silent_chunks : 0;
}

void silence_counter_init(silence_counter_t* counter, double threshold_db,
                          double timeout_seconds, size_t samplerate,
                          size_t chunksize) {
  if (!counter) return;
  counter->threshold_db = threshold_db;
  counter->silent_chunks = 0;
  // Convert the timeout duration from seconds to the number of audio chunks.
  if (timeout_seconds > 0.0 && chunksize > 0) {
    counter->limit_chunks = (size_t)round(
        (timeout_seconds * (double)samplerate) / (double)chunksize);
  } else {
    counter->limit_chunks = 0;
  }
}

/// Feed the next chunk's loudest channel peak (dB). Returns the
/// engine state the capture loop should drive to.
processing_state_t silence_counter_update(silence_counter_t* counter,
                                          double signal_peak_db) {
  if (!counter || counter->limit_chunks == 0) {
    return PROCESSING_STATE_RUNNING;
  }
  // Reset counter if signal level is above the silence threshold.
  if (signal_peak_db > counter->threshold_db) {
    counter->silent_chunks = 0;
    return PROCESSING_STATE_RUNNING;
  }
  // Increment silent chunk count, bounding it to the limit.
  if (counter->silent_chunks < counter->limit_chunks) {
    counter->silent_chunks++;
  }
  // Transition to PAUSED state if silence duration exceeds the limit.
  return (counter->silent_chunks >= counter->limit_chunks)
             ? PROCESSING_STATE_PAUSED
             : PROCESSING_STATE_RUNNING;
}
