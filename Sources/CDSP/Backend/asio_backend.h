#ifndef CLIB_BACKEND_ASIO_BACKEND_H
#define CLIB_BACKEND_ASIO_BACKEND_H

#if defined(ENABLE_ASIO)

#include "audio_backend.h"

typedef struct asio_capture asio_capture_t;
typedef struct asio_playback asio_playback_t;

/// Create ASIO capture backend
capture_backend_t* asio_capture_new(const capture_device_config_t* config,
                                    int sample_rate, int chunk_size,
                                    bool full_duplex, backend_error_t* err);
/// Create ASIO playback backend
playback_backend_t* asio_playback_new(const playback_device_config_t* config,
                                      int sample_rate, int chunk_size,
                                      bool full_duplex, backend_error_t* err);

#endif  // ENABLE_ASIO

#endif  // CLIB_BACKEND_ASIO_BACKEND_H
