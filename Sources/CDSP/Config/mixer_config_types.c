#include "mixer_config_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Standalone mixer configuration types.

// Support both nested format `channels: { in: N, out: N }` and
// flat format `channels_in: N, channels_out: N`
// Try nested format first: channels: { in: N, out: N }
// Fall back to flat format: channels_in / channels_out
// Encode in the nested format

/// Convenience accessor: 0.0 when gain is nil
double mixer_source_gain_value(const mixer_source_t* src) {
  if (!src || !src->has_gain) return 0.0;
  return src->gain;
}

/// Validate the mapping is internally consistent: every dest is in
/// range, no dest appears twice, and within a single dest no source
/// channel appears twice.
int mixer_config_validate(const mixer_config_t* mixer, config_error_t* err) {
  if (!mixer) return 0;

  // Validate the mapping is internally consistent: every dest is in
  // range, no dest appears twice, and within a single dest no source
  // channel appears twice.

  // Allocate a tracking array for destination channels to detect duplicates.
  // We allocate at least 1 element to avoid passing 0 to calloc if channels_out
  // is 0.
  bool* seen_dests = (bool*)calloc(
      mixer->channels_out > 0 ? mixer->channels_out : 1, sizeof(bool));
  if (!seen_dests) return -1;

  for (size_t i = 0; i < mixer->mapping_count; i++) {
    int dest = mixer->mapping[i].dest;
    // Ensure destination channel index is within the configured output
    // channels.
    if ((size_t)dest >= mixer->channels_out) {
      config_error_set(err, CONFIG_ERR_INVALID_MIXER,
                       "mixer dest %d >= channels_out %d", dest,
                       mixer->channels_out);
      free(seen_dests);
      return -1;
    }
    // Detect if the same destination channel is mapped multiple times.
    if (seen_dests[dest]) {
      config_error_set(err, CONFIG_ERR_INVALID_MIXER,
                       "mixer dest %d mapped more than once", dest);
      free(seen_dests);
      return -1;
    }
    seen_dests[dest] = true;

    // Allocate a tracking array for source channels for the current destination
    // to detect duplicate source channels.
    bool* seen_sources = (bool*)calloc(
        mixer->channels_in > 0 ? mixer->channels_in : 1, sizeof(bool));
    if (!seen_sources) {
      free(seen_dests);
      return -1;
    }
    for (size_t j = 0; j < mixer->mapping[i].sources_count; j++) {
      int src_ch = mixer->mapping[i].sources[j].channel;
      // Ensure source channel index is within the configured input channels.
      if ((size_t)src_ch >= mixer->channels_in) {
        config_error_set(err, CONFIG_ERR_INVALID_MIXER,
                         "mixer source channel %d >= channels_in %d", src_ch,
                         mixer->channels_in);
        free(seen_sources);
        free(seen_dests);
        return -1;
      }
      // Detect if the same source channel is added multiple times to the same
      // destination.
      if (seen_sources[src_ch]) {
        config_error_set(
            err, CONFIG_ERR_INVALID_MIXER,
            "mixer source channel %d listed more than once for dest %d", src_ch,
            dest);
        free(seen_sources);
        free(seen_dests);
        return -1;
      }
      seen_sources[src_ch] = true;
    }
    free(seen_sources);
  }

  free(seen_dests);
  return 0;
}
