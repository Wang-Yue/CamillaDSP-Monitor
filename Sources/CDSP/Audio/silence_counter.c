// SilenceCounter — counts consecutive silent chunks against a dB threshold.
#include "Audio/silence_counter.h"

#include <math.h>

void silence_counter_init(silence_counter_t* counter, double threshold_db,
                          double timeout_seconds, size_t samplerate,
                          size_t chunksize) {
  if (!counter) return;
  counter->threshold_db = threshold_db;
  counter->silent_chunks = 0;
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
  if (signal_peak_db > counter->threshold_db) {
    counter->silent_chunks = 0;
    return PROCESSING_STATE_RUNNING;
  }
  if (counter->silent_chunks < counter->limit_chunks) {
    counter->silent_chunks++;
  }
  return (counter->silent_chunks >= counter->limit_chunks)
             ? PROCESSING_STATE_PAUSED
             : PROCESSING_STATE_RUNNING;
}
