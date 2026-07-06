// SilenceCounter — counts consecutive silent chunks against a dB threshold.

#ifndef CLIB_AUDIO_SILENCE_COUNTER_H
#define CLIB_AUDIO_SILENCE_COUNTER_H

#include <stddef.h>

#include "Config/engine_config_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Counts consecutive silent chunks against a dB threshold and
/// reports back the desired engine state. `silence_counter_update(...)`
/// returns `PROCESSING_STATE_PAUSED` once silence has persisted for at least
/// the configured timeout, `PROCESSING_STATE_RUNNING` otherwise.
///
/// Disabled when `timeout_seconds <= 0` — in that case `update`
/// always returns `PROCESSING_STATE_RUNNING`.
typedef struct {
  size_t limit_chunks;
  double threshold_db;
  size_t silent_chunks;
} silence_counter_t;

void silence_counter_init(silence_counter_t* counter, double threshold_db,
                          double timeout_seconds, size_t samplerate,
                          size_t chunksize);

/// Feed the next chunk's loudest channel peak (dB). Returns the
/// engine state the capture loop should drive to.
processing_state_t silence_counter_update(silence_counter_t* counter,
                                          double signal_peak_db);

#ifdef __cplusplus
}
#endif

#endif  // CLIB_AUDIO_SILENCE_COUNTER_H
