#ifndef CLIB_BACKEND_ALSA_DEVICE_H
#define CLIB_BACKEND_ALSA_DEVICE_H

#if defined(ENABLE_ALSA)

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern pthread_mutex_t g_alsa_mutex;

#endif  // ENABLE_ALSA

#endif  // CLIB_BACKEND_ALSA_DEVICE_H
