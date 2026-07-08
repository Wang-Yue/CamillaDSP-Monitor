#ifndef CLIB_BACKEND_ALSA_DEVICE_H
#define CLIB_BACKEND_ALSA_DEVICE_H

#if defined(ENABLE_ALSA)

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file alsa_device.h
 * @brief Provides shared ALSA device resources, such as mutexes.
 */

/**
 * @brief Global mutex for protecting ALSA API calls.
 *
 * Since ALSA functions are not all thread-safe or need synchronization
 * when accessed from multiple threads (e.g. playback thread vs control thread),
 * this mutex should be locked before performing ALSA operations.
 */
extern pthread_mutex_t g_alsa_mutex;

#endif  // ENABLE_ALSA

#endif  // CLIB_BACKEND_ALSA_DEVICE_H
