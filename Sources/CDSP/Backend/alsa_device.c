#if defined(ENABLE_ALSA)
#include "alsa_device.h"

#include <pthread.h>

pthread_mutex_t g_alsa_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
