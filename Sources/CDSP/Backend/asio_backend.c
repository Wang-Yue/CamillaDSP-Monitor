#if defined(ENABLE_ASIO)

#define WIN32_LEAN_AND_MEAN
#include "asio_backend.h"

#include <initguid.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unknwn.h>
#include <windows.h>

#include "Audio/lock_free_ring_buffer.h"
#include "Logging/app_logger.h"

// COM Release helper
static bool find_asio_driver_clsid(const char* driver_name, CLSID* out_clsid);
static void asio_capture_close_internal(void* ctx);
static void asio_playback_close_internal(void* ctx);

#define SAFE_RELEASE(punk)         \
  if ((punk) != NULL) {            \
    (punk)->lpVtbl->Release(punk); \
    (punk) = NULL;                 \
  }

// ASIO type definitions
typedef int32_t ASIOBool;
#define ASIOFalse 0
#define ASIOTrue 1

typedef double ASIOSampleRate;
typedef long ASIOError;

typedef enum {
  ASIOSTInt16MSB = 0,
  ASIOSTInt24MSB = 1,
  ASIOSTInt32MSB = 2,
  ASIOSTFloat32MSB = 3,
  ASIOSTFloat64MSB = 4,
  ASIOSTInt32MSB16 = 8,
  ASIOSTInt32MSB18 = 9,
  ASIOSTInt32MSB20 = 10,
  ASIOSTInt32MSB24 = 11,
  ASIOSTInt16LSB = 16,
  ASIOSTInt24LSB = 17,
  ASIOSTInt32LSB = 18,
  ASIOSTFloat32LSB = 19,
  ASIOSTFloat64LSB = 20,
  ASIOSTInt32LSB16 = 24,
  ASIOSTInt32LSB18 = 25,
  ASIOSTInt32LSB20 = 26,
  ASIOSTInt32LSB24 = 27,
} ASIOSampleType;

typedef struct {
  int32_t channel;
  ASIOBool isInput;
  ASIOBool isActive;
  int32_t channelGroup;
  int32_t type;
  char name[32];
} ASIOChannelInfo;

typedef struct {
  ASIOBool isInput;
  int32_t channelNum;
  void* buffers[2];
} ASIOBufferInfo;

typedef struct {
  void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
  void (*sampleRateDidChange)(ASIOSampleRate sRate);
  long (*asioMessage)(long selector, long value, void* message, double* opt);
  void* (*bufferSwitchTimeInfo)(void* params, long doubleBufferIndex,
                                ASIOBool directProcess);
} ASIOCallbacks;

static ASIOCallbacks asio_callbacks;

// Forward declaration of COM interface
typedef struct IASIO IASIO;
static bool force_sample_rate_with_dummy_cycle(const char* driver_name,
                                               IASIO** p_iasio, double rate,
                                               backend_error_t* err);
typedef struct IASIOVtbl {
  HRESULT(STDMETHODCALLTYPE* QueryInterface)(IASIO* This, REFIID riid,
                                             void** ppvObject);
  ULONG(STDMETHODCALLTYPE* AddRef)(IASIO* This);
  ULONG(STDMETHODCALLTYPE* Release)(IASIO* This);
  ASIOBool(STDMETHODCALLTYPE* init)(IASIO* This, void* sysHandle);
  void(STDMETHODCALLTYPE* getDriverName)(IASIO* This, char* name);
  long(STDMETHODCALLTYPE* getDriverVersion)(IASIO* This);
  void(STDMETHODCALLTYPE* getErrorMessage)(IASIO* This, char* string);
  ASIOError(STDMETHODCALLTYPE* start)(IASIO* This);
  ASIOError(STDMETHODCALLTYPE* stop)(IASIO* This);
  ASIOError(STDMETHODCALLTYPE* getChannels)(IASIO* This, long* numInputChannels,
                                            long* numOutputChannels);
  ASIOError(STDMETHODCALLTYPE* getLatencies)(IASIO* This, long* inputLatency,
                                             long* outputLatency);
  ASIOError(STDMETHODCALLTYPE* getBufferSize)(IASIO* This, long* minSize,
                                              long* maxSize,
                                              long* preferredSize,
                                              long* granularity);
  ASIOError(STDMETHODCALLTYPE* canSampleRate)(IASIO* This, double sampleRate);
  ASIOError(STDMETHODCALLTYPE* getSampleRate)(IASIO* This, double* sampleRate);
  ASIOError(STDMETHODCALLTYPE* setSampleRate)(IASIO* This, double sampleRate);
  ASIOError(STDMETHODCALLTYPE* getClockSources)(IASIO* This, void* clocks,
                                                long* numSources);
  ASIOError(STDMETHODCALLTYPE* setClockSource)(IASIO* This, long reference);
  ASIOError(STDMETHODCALLTYPE* getSamplePosition)(IASIO* This, int64_t* sPos,
                                                  int64_t* tStamp);
  ASIOError(STDMETHODCALLTYPE* getChannelInfo)(IASIO* This, void* info);
  ASIOError(STDMETHODCALLTYPE* createBuffers)(IASIO* This, void* bufferInfos,
                                              long numChannels, long bufferSize,
                                              void* callbacks);
  ASIOError(STDMETHODCALLTYPE* disposeBuffers)(IASIO* This);
  ASIOError(STDMETHODCALLTYPE* controlPanel)(IASIO* This);
  ASIOError(STDMETHODCALLTYPE* future)(IASIO* This, long selector, void* opt);
  ASIOError(STDMETHODCALLTYPE* outputReady)(IASIO* This);
} IASIOVtbl;

struct IASIO {
  const IASIOVtbl* lpVtbl;
};

// Internal structures
struct asio_capture {
  char device[256];
  int channels;
  int sample_rate;
  int chunk_size;
  asio_sample_format_t format;

  IASIO* iasio;
  spsc_audio_ring_buffer_t* ring_buffer;
  float* decode_buf;
  size_t decode_buf_size;

  ASIOBufferInfo* buffer_infos;
  ASIOChannelInfo* channel_infos;
  long actual_buffer_size;
  bool is_running;
  bool full_duplex;
  bool com_initialized;

  float* callback_buf;
  size_t callback_buf_size;
};

struct asio_playback {
  char device[256];
  int channels;
  int sample_rate;
  int chunk_size;
  asio_sample_format_t format;

  IASIO* iasio;
  spsc_audio_ring_buffer_t* ring_buffer;
  float* encode_buf;
  size_t encode_buf_size;

  ASIOBufferInfo* buffer_infos;
  ASIOChannelInfo* channel_infos;
  long actual_buffer_size;
  bool is_running;
  bool full_duplex;
  bool com_initialized;

  float* callback_buf;
  size_t callback_buf_size;
};

// Global active backend references
static asio_capture_t* g_active_capture = NULL;
static asio_playback_t* g_active_playback = NULL;
static HANDLE g_capture_event = NULL;

typedef struct {
  SRWLOCK lock;
  CONDITION_VARIABLE cond;
  bool initialized;
  char driver_name[256];
  int sample_rate;
  int preferred_buf_size;
  int num_inputs;
  int num_outputs;
  IASIO* iasio;

  // Registered sides
  ASIOBufferInfo* playback_buffer_infos;
  ASIOChannelInfo* playback_channel_infos;
  int playback_channels;
  bool playback_ready;

  ASIOBufferInfo* capture_buffer_infos;
  ASIOChannelInfo* capture_channel_infos;
  int capture_channels;
  bool capture_ready;

  // Coordination
  bool stream_started;
  char setup_error[256];
  int active_count;

  // Combined structures passed to createBuffers
  ASIOBufferInfo* combined_buffer_infos;
  ASIOChannelInfo* combined_channel_infos;
  int combined_channels;
} asio_shared_state_t;

static asio_shared_state_t g_asio_shared = {.lock = SRWLOCK_INIT,
                                            .cond = CONDITION_VARIABLE_INIT,
                                            .initialized = false,
                                            .iasio = NULL};

/**
 * @brief Coordinates ASIO initialization for full-duplex setups.
 *
 * Registers either the capture or playback side and waits for the other side to
 * register. Once both are ready, it allocates combined buffers and starts the
 * ASIO stream. For the second arriving side, it waits for the first side to
 * complete setup.
 *
 * @param is_input True if registering for capture, false for playback.
 * @param driver_name Name of the ASIO driver.
 * @param sample_rate Requested sample rate.
 * @param channels Number of channels.
 * @param format Audio sample format.
 * @param out_iasio Pointer to receive the shared IASIO interface.
 * @param out_buffer_infos Pointer to receive the buffer info array.
 * @param out_channel_infos Pointer to receive the channel info array.
 * @param out_buf_size Pointer to receive the buffer size.
 * @param err Pointer to backend_error_t to receive error details.
 * @return true if successful, false otherwise.
 */
static bool register_and_wait_asio(bool is_input, const char* driver_name,
                                   int sample_rate, int channels,
                                   asio_sample_format_t format,
                                   IASIO** out_iasio,
                                   ASIOBufferInfo** out_buffer_infos,
                                   ASIOChannelInfo** out_channel_infos,
                                   long* out_buf_size, backend_error_t* err) {
  (void)format;
  AcquireSRWLockExclusive(&g_asio_shared.lock);

  if (!g_asio_shared.initialized) {
    g_asio_shared.initialized = true;
    snprintf(g_asio_shared.driver_name, sizeof(g_asio_shared.driver_name), "%s",
             driver_name);
    g_asio_shared.sample_rate = sample_rate;
    g_asio_shared.stream_started = false;
    g_asio_shared.setup_error[0] = '\0';
    g_asio_shared.active_count = 0;
    g_asio_shared.playback_ready = false;
    g_asio_shared.capture_ready = false;
    g_asio_shared.combined_buffer_infos = NULL;
    g_asio_shared.combined_channel_infos = NULL;
    g_asio_shared.combined_channels = 0;

    CLSID clsid;
    if (!find_asio_driver_clsid(driver_name, &clsid)) {
      g_asio_shared.initialized = false;
      ReleaseSRWLockExclusive(&g_asio_shared.lock);
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "ASIO driver CLSID not found");
      return false;
    }

    HRESULT hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &clsid,
                                  (void**)&g_asio_shared.iasio);
    if (FAILED(hr)) {
      g_asio_shared.initialized = false;
      ReleaseSRWLockExclusive(&g_asio_shared.lock);
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to create CLSID instance");
      return false;
    }

    if (!g_asio_shared.iasio->lpVtbl->init(g_asio_shared.iasio,
                                           GetDesktopWindow())) {
      g_asio_shared.iasio->lpVtbl->Release(g_asio_shared.iasio);
      g_asio_shared.iasio = NULL;
      g_asio_shared.initialized = false;
      ReleaseSRWLockExclusive(&g_asio_shared.lock);
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "ASIO init failed");
      return false;
    }

    long min_sz, max_sz, pref_sz, granularity;
    g_asio_shared.iasio->lpVtbl->getBufferSize(g_asio_shared.iasio, &min_sz,
                                               &max_sz, &pref_sz, &granularity);
    g_asio_shared.preferred_buf_size = pref_sz;

    long num_in, num_out;
    g_asio_shared.iasio->lpVtbl->getChannels(g_asio_shared.iasio, &num_in,
                                             &num_out);
    g_asio_shared.num_inputs = num_in;
    g_asio_shared.num_outputs = num_out;

    // Set requested sample rate
    double rate = sample_rate;
    double current_rate = 0.0;
    if (g_asio_shared.iasio->lpVtbl->getSampleRate(g_asio_shared.iasio,
                                                   &current_rate) == 0) {
      if (fabs(current_rate - rate) > 0.5) {
        if (g_asio_shared.iasio->lpVtbl->setSampleRate(g_asio_shared.iasio,
                                                       rate) == 0) {
          // Force sample rate change via dummy stream cycle
          if (!force_sample_rate_with_dummy_cycle(
                  driver_name, &g_asio_shared.iasio, rate, err)) {
            g_asio_shared.initialized = false;
            ReleaseSRWLockExclusive(&g_asio_shared.lock);
            return false;
          }
        } else {
          g_asio_shared.initialized = false;
          ReleaseSRWLockExclusive(&g_asio_shared.lock);
          if (err)
            backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                               "Failed to set ASIO sample rate");
          return false;
        }
      }
    }
  } else {
    if (strcmp(g_asio_shared.driver_name, driver_name) != 0) {
      ReleaseSRWLockExclusive(&g_asio_shared.lock);
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "ASIO driver name mismatch");
      return false;
    }
  }

  if (g_asio_shared.setup_error[0] != '\0') {
    ReleaseSRWLockExclusive(&g_asio_shared.lock);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         g_asio_shared.setup_error);
    return false;
  }

  ASIOBufferInfo* buf_infos =
      (ASIOBufferInfo*)calloc(channels, sizeof(ASIOBufferInfo));
  ASIOChannelInfo* chan_infos =
      (ASIOChannelInfo*)calloc(channels, sizeof(ASIOChannelInfo));

  if (is_input) {
    g_asio_shared.capture_buffer_infos = buf_infos;
    g_asio_shared.capture_channel_infos = chan_infos;
    g_asio_shared.capture_channels = channels;
    g_asio_shared.capture_ready = true;

    for (int i = 0; i < channels; i++) {
      buf_infos[i].isInput = ASIOTrue;
      buf_infos[i].channelNum = i;
      chan_infos[i].channel = i;
      chan_infos[i].isInput = ASIOTrue;
      g_asio_shared.iasio->lpVtbl->getChannelInfo(g_asio_shared.iasio,
                                                  &chan_infos[i]);
    }
  } else {
    g_asio_shared.playback_buffer_infos = buf_infos;
    g_asio_shared.playback_channel_infos = chan_infos;
    g_asio_shared.playback_channels = channels;
    g_asio_shared.playback_ready = true;

    for (int i = 0; i < channels; i++) {
      buf_infos[i].isInput = ASIOFalse;
      buf_infos[i].channelNum = i;
      chan_infos[i].channel = i;
      chan_infos[i].isInput = ASIOFalse;
      g_asio_shared.iasio->lpVtbl->getChannelInfo(g_asio_shared.iasio,
                                                  &chan_infos[i]);
    }
  }

  if (g_asio_shared.playback_ready && g_asio_shared.capture_ready) {
    int pb_ch = g_asio_shared.playback_channels;
    int cap_ch = g_asio_shared.capture_channels;
    int total_ch = pb_ch + cap_ch;

    g_asio_shared.combined_buffer_infos =
        (ASIOBufferInfo*)calloc(total_ch, sizeof(ASIOBufferInfo));
    g_asio_shared.combined_channel_infos =
        (ASIOChannelInfo*)calloc(total_ch, sizeof(ASIOChannelInfo));
    g_asio_shared.combined_channels = total_ch;

    memcpy(g_asio_shared.combined_buffer_infos,
           g_asio_shared.playback_buffer_infos, pb_ch * sizeof(ASIOBufferInfo));
    memcpy(g_asio_shared.combined_channel_infos,
           g_asio_shared.playback_channel_infos,
           pb_ch * sizeof(ASIOChannelInfo));

    memcpy(g_asio_shared.combined_buffer_infos + pb_ch,
           g_asio_shared.capture_buffer_infos, cap_ch * sizeof(ASIOBufferInfo));
    memcpy(g_asio_shared.combined_channel_infos + pb_ch,
           g_asio_shared.capture_channel_infos,
           cap_ch * sizeof(ASIOChannelInfo));

    ASIOError create_res = g_asio_shared.iasio->lpVtbl->createBuffers(
        g_asio_shared.iasio, g_asio_shared.combined_buffer_infos, total_ch,
        g_asio_shared.preferred_buf_size, &asio_callbacks);
    if (create_res != 0) {
      snprintf(g_asio_shared.setup_error, sizeof(g_asio_shared.setup_error),
               "ASIOCreateBuffers failed in full-duplex setup: %ld",
               create_res);
      WakeAllConditionVariable(&g_asio_shared.cond);
      ReleaseSRWLockExclusive(&g_asio_shared.lock);
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           g_asio_shared.setup_error);
      return false;
    }

    for (int i = 0; i < pb_ch; i++) {
      g_asio_shared.playback_buffer_infos[i].buffers[0] =
          g_asio_shared.combined_buffer_infos[i].buffers[0];
      g_asio_shared.playback_buffer_infos[i].buffers[1] =
          g_asio_shared.combined_buffer_infos[i].buffers[1];
    }
    for (int i = 0; i < cap_ch; i++) {
      g_asio_shared.capture_buffer_infos[i].buffers[0] =
          g_asio_shared.combined_buffer_infos[pb_ch + i].buffers[0];
      g_asio_shared.capture_buffer_infos[i].buffers[1] =
          g_asio_shared.combined_buffer_infos[pb_ch + i].buffers[1];
    }

    ASIOError start_res =
        g_asio_shared.iasio->lpVtbl->start(g_asio_shared.iasio);
    if (start_res != 0) {
      snprintf(g_asio_shared.setup_error, sizeof(g_asio_shared.setup_error),
               "ASIOStart failed in full-duplex setup: %ld", start_res);
      g_asio_shared.iasio->lpVtbl->disposeBuffers(g_asio_shared.iasio);
      WakeAllConditionVariable(&g_asio_shared.cond);
      ReleaseSRWLockExclusive(&g_asio_shared.lock);
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           g_asio_shared.setup_error);
      return false;
    }

    g_asio_shared.stream_started = true;
    g_asio_shared.active_count = 2;
    WakeAllConditionVariable(&g_asio_shared.cond);
  }

  *out_iasio = g_asio_shared.iasio;
  *out_buf_size = g_asio_shared.preferred_buf_size;
  *out_buffer_infos = buf_infos;
  *out_channel_infos = chan_infos;

  ReleaseSRWLockExclusive(&g_asio_shared.lock);
  return true;
}

/**
 * @brief Releases the shared ASIO driver instance in full-duplex mode.
 *
 * Decrements the active usage count. When count drops to 1, stops the stream.
 * When count drops to 0, disposes of buffers and releases the COM interface,
 * resetting the shared state.
 *
 * @param is_input True if releasing from the capture side, false for playback.
 * @param iasio The IASIO driver interface.
 */
static void release_shared_asio(bool is_input, IASIO* iasio) {
  AcquireSRWLockExclusive(&g_asio_shared.lock);
  if (g_asio_shared.initialized) {
    if (is_input) {
      g_asio_shared.capture_ready = false;
    } else {
      g_asio_shared.playback_ready = false;
    }
    g_asio_shared.active_count--;
    if (g_asio_shared.active_count == 1) {
      iasio->lpVtbl->stop(iasio);
      g_active_capture = NULL;
      g_active_playback = NULL;
    } else if (g_asio_shared.active_count == 0) {
      iasio->lpVtbl->disposeBuffers(iasio);
      iasio->lpVtbl->Release(iasio);

      if (g_asio_shared.combined_buffer_infos)
        free(g_asio_shared.combined_buffer_infos);
      if (g_asio_shared.combined_channel_infos)
        free(g_asio_shared.combined_channel_infos);

      memset(&g_asio_shared, 0, sizeof(g_asio_shared));
      InitializeSRWLock(&g_asio_shared.lock);
      InitializeConditionVariable(&g_asio_shared.cond);
    }
  }
  ReleaseSRWLockExclusive(&g_asio_shared.lock);
}

/**
 * @brief Forces a sample rate change on some problematic ASIO drivers.
 *
 * Certain ASIO drivers do not apply sample rate changes until a stream cycle
 * has been initiated. This function runs a short dummy cycle (creates a buffer,
 * starts, sleeps, stops, destroys, recreates the driver) to enforce the rate.
 *
 * @param driver_name Name of the ASIO driver.
 * @param p_iasio Pointer to the IASIO driver interface pointer (may be
 * recreated).
 * @param rate Target sample rate.
 * @param err Pointer to backend_error_t to receive error details.
 * @return true if successful, false otherwise.
 */
static bool force_sample_rate_with_dummy_cycle(const char* driver_name,
                                               IASIO** p_iasio, double rate,
                                               backend_error_t* err) {
  IASIO* iasio = *p_iasio;

  long num_in = 0, num_out = 0;
  ASIOError res = iasio->lpVtbl->getChannels(iasio, &num_in, &num_out);
  if (res != 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "ASIO getChannels failed during dummy cycle");
    return false;
  }

  long min_sz, max_sz, pref_sz, granularity;
  res = iasio->lpVtbl->getBufferSize(iasio, &min_sz, &max_sz, &pref_sz,
                                     &granularity);
  if (res != 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "ASIO getBufferSize failed during dummy cycle");
    return false;
  }

  bool is_input = (num_out == 0);
  ASIOBufferInfo dummy_buf;
  memset(&dummy_buf, 0, sizeof(dummy_buf));
  dummy_buf.isInput = is_input ? ASIOTrue : ASIOFalse;
  dummy_buf.channelNum = 0;

  res = iasio->lpVtbl->createBuffers(iasio, &dummy_buf, 1, pref_sz,
                                     &asio_callbacks);
  if (res != 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "ASIO createBuffers failed during dummy cycle");
    return false;
  }

  res = iasio->lpVtbl->start(iasio);
  if (res != 0) {
    iasio->lpVtbl->disposeBuffers(iasio);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "ASIO start failed during dummy cycle");
    return false;
  }

  Sleep(50);

  iasio->lpVtbl->stop(iasio);
  iasio->lpVtbl->disposeBuffers(iasio);
  SAFE_RELEASE(iasio);
  *p_iasio = NULL;

  Sleep(50);

  CLSID clsid;
  if (!find_asio_driver_clsid(driver_name, &clsid)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to find driver CLSID after dummy cycle");
    return false;
  }

  HRESULT hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &clsid,
                                (void**)&iasio);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(
          err, BACKEND_ERROR_INITIALIZATION_FAILED,
          "Failed to recreate driver instance after dummy cycle");
    return false;
  }

  if (!iasio->lpVtbl->init(iasio, GetDesktopWindow())) {
    SAFE_RELEASE(iasio);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to re-initialize driver after dummy cycle");
    return false;
  }

  res = iasio->lpVtbl->setSampleRate(iasio, rate);
  if (res != 0) {
    SAFE_RELEASE(iasio);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to set rate after dummy cycle");
    return false;
  }

  double verify = 0.0;
  res = iasio->lpVtbl->getSampleRate(iasio, &verify);
  if (res != 0) {
    SAFE_RELEASE(iasio);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to read rate after dummy cycle");
    return false;
  }

  if (fabs(verify - rate) > 0.5) {
    SAFE_RELEASE(iasio);
    if (err)
      backend_error_init(
          err, BACKEND_ERROR_INITIALIZATION_FAILED,
          "ASIO sample rate verification failed after dummy cycle");
    return false;
  }

  *p_iasio = iasio;
  return true;
}

/**
 * @brief Searches the Windows Registry to find the CLSID of an ASIO driver by
 * name.
 *
 * @param driver_name Name of the ASIO driver.
 * @param out_clsid Pointer to a CLSID structure to receive the result.
 * @return true if the CLSID was successfully found, false otherwise.
 */
static bool find_asio_driver_clsid(const char* driver_name, CLSID* out_clsid) {
  HKEY hk;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\ASIO", 0, KEY_READ, &hk) !=
      ERROR_SUCCESS) {
    return false;
  }

  char subkey_name[256];
  DWORD index = 0;
  bool found = false;

  while (RegEnumKeyA(hk, index++, subkey_name, sizeof(subkey_name)) ==
         ERROR_SUCCESS) {
    if (driver_name[0] == '\0' || strcasecmp(subkey_name, driver_name) == 0 ||
        strstr(subkey_name, driver_name) != NULL) {
      HKEY hk_driver;
      if (RegOpenKeyExA(hk, subkey_name, 0, KEY_READ, &hk_driver) ==
          ERROR_SUCCESS) {
        char clsid_str[128];
        DWORD size = sizeof(clsid_str);
        if (RegQueryValueExA(hk_driver, "CLSID", NULL, NULL, (LPBYTE)clsid_str,
                             &size) == ERROR_SUCCESS) {
          wchar_t wclsid_str[128];
          mbstowcs(wclsid_str, clsid_str, 128);
          if (SUCCEEDED(CLSIDFromString(wclsid_str, out_clsid))) {
            found = true;
          }
        }
        RegCloseKey(hk_driver);
      }
      if (found) break;
    }
  }
  RegCloseKey(hk);
  return found;
}

// ASIO Callback implementation
/**
 * @brief ASIO buffer switch callback.
 *
 * Called by the ASIO driver when a buffer needs to be processed.
 * It reads playback data from the playback ring buffer, converts it to the
 * native ASIO format, and copies it to the active ASIO driver buffer. It also
 * reads capture data from the active ASIO driver buffer, converts it to float,
 * and writes it to the capture ring buffer.
 *
 * @param doubleBufferIndex Index of the active buffer (0 or 1).
 * @param directProcess Indicates if the driver is in direct process mode.
 */
static void asio_buffer_switch(long doubleBufferIndex, ASIOBool directProcess) {
  (void)directProcess;

  // Playback phase
  if (g_active_playback && g_active_playback->is_running) {
    long frames = g_active_playback->actual_buffer_size;
    int channels = g_active_playback->channels;

    float* interleaved_buf = g_active_playback->callback_buf;
    if (interleaved_buf) {
      size_t read_samples = spsc_audio_ring_buffer_consume(
          g_active_playback->ring_buffer, interleaved_buf, frames * channels);
      if (read_samples < (size_t)(frames * channels)) {
        memset(interleaved_buf + read_samples, 0,
               (frames * channels - read_samples) * sizeof(float));
      }

      long num_in = 0, num_out = 0;
      g_active_playback->iasio->lpVtbl->getChannels(g_active_playback->iasio,
                                                    &num_in, &num_out);

      for (int c = 0; c < channels; c++) {
        // Output channel indexes start after the input channels in ASIO.
        int buf_idx = num_in + c;
        void* dst =
            g_active_playback->buffer_infos[buf_idx].buffers[doubleBufferIndex];
        int type = g_active_playback->channel_infos[buf_idx].type;

        for (long f = 0; f < frames; f++) {
          float val = interleaved_buf[f * channels + c];
          // Clip float sample to [-1.0, 1.0] before conversion.
          if (val > 1.0f)
            val = 1.0f;
          else if (val < -1.0f)
            val = -1.0f;

          // Convert sample based on the ASIO channel's native format.
          if (type == ASIOSTInt16LSB) {
            ((int16_t*)dst)[f] = (int16_t)(val * 32767.0f);
          } else if (type == ASIOSTInt32LSB || type == ASIOSTInt32LSB16 ||
                     type == ASIOSTInt32LSB18 || type == ASIOSTInt32LSB20 ||
                     type == ASIOSTInt32LSB24) {
            ((int32_t*)dst)[f] = (int32_t)(val * 2147483647.0f);
          } else if (type == ASIOSTFloat32LSB) {
            ((float*)dst)[f] = val;
          } else if (type == ASIOSTFloat64LSB) {
            ((double*)dst)[f] = (double)val;
          } else if (type == ASIOSTInt24LSB) {
            // ASIOSTInt24LSB uses packed 3-byte samples.
            int32_t ival = (int32_t)(val * 8388607.0f);
            uint8_t* p = &((uint8_t*)dst)[f * 3];
            p[0] = ival & 0xFF;
            p[1] = (ival >> 8) & 0xFF;
            p[2] = (ival >> 16) & 0xFF;
          }
        }
      }
    }
  }

  // Capture phase
  if (g_active_capture && g_active_capture->is_running) {
    long frames = g_active_capture->actual_buffer_size;
    int channels = g_active_capture->channels;

    float* interleaved_buf = g_active_capture->callback_buf;
    if (interleaved_buf) {
      for (int c = 0; c < channels; c++) {
        void* src =
            g_active_capture->buffer_infos[c].buffers[doubleBufferIndex];
        int type = g_active_capture->channel_infos[c].type;

        for (long f = 0; f < frames; f++) {
          float val = 0.0f;
          // Convert the native ASIO channel's samples to float.
          if (type == ASIOSTInt16LSB) {
            val = ((int16_t*)src)[f] / 32768.0f;
          } else if (type == ASIOSTInt32LSB || type == ASIOSTInt32LSB16 ||
                     type == ASIOSTInt32LSB18 || type == ASIOSTInt32LSB20 ||
                     type == ASIOSTInt32LSB24) {
            val = ((int32_t*)src)[f] / 2147483648.0f;
          } else if (type == ASIOSTFloat32LSB) {
            val = ((float*)src)[f];
          } else if (type == ASIOSTFloat64LSB) {
            val = (float)(((double*)src)[f]);
          } else if (type == ASIOSTInt24LSB) {
            // ASIOSTInt24LSB uses packed 3-byte samples. Sign-extend the value.
            uint8_t* p = &((uint8_t*)src)[f * 3];
            int32_t ival = (p[0]) | (p[1] << 8) | (p[2] << 16);
            if (ival & 0x800000) ival |= 0xFF000000;
            val = ival / 8388608.0f;
          }
          interleaved_buf[f * channels + c] = val;
        }
      }

      spsc_audio_ring_buffer_write(g_active_capture->ring_buffer,
                                   interleaved_buf, frames * channels, 1);
      if (g_capture_event) SetEvent(g_capture_event);
    }
  }
}

/**
 * @brief ASIO sample rate change notification callback.
 *
 * @param sRate The new sample rate.
 */
static void asio_sample_rate_did_change(ASIOSampleRate sRate) { (void)sRate; }

/**
 * @brief ASIO driver message callback.
 *
 * Handles control messages from the ASIO driver (e.g. reset requests, buffer
 * size changes).
 *
 * @param selector Message type selector.
 * @param value Selector-specific value.
 * @param message Selector-specific pointer.
 * @param opt Optional selector-specific parameter.
 * @return Response code specific to the message.
 */
static long asio_message(long selector, long value, void* message,
                         double* opt) {
  (void)message;
  (void)opt;
  switch (selector) {
    case 1:  // kAsioSelectorSupported
      switch (value) {
        case 2:  // kAsioEngineVersion
        case 5:  // kAsioResetRequest
        case 6:  // kAsioBufferSizeChange
        case 7:  // kAsioResyncRequest
        case 8:  // kAsioLatenciesChanged
        case 3:  // kAsioSupportsTimeInfo
          return 1;
        default:
          return 0;
      }
    case 2:      // kAsioEngineVersion
      return 2;  // ASIO 2.0
    case 3:      // kAsioSupportsTimeInfo
      return 1;
    case 5:  // kAsioResetRequest {
    {
      logger_t logger = logger_create("dsp.backend.asio");
      logger_warn(&logger, "ASIO reset request received from driver.",
                  log_arg_none(), log_arg_none(), log_arg_none(),
                  log_arg_none());
    }
      return 1;
    case 6:  // kAsioBufferSizeChange
      return 1;
    case 7:  // kAsioResyncRequest
      return 1;
    case 8:  // kAsioLatenciesChanged
      return 1;
    default:
      return 0;
  }
}

/**
 * @brief ASIO buffer switch time info callback.
 *
 * Wrapper callback that invokes asio_buffer_switch.
 *
 * @param params User parameters.
 * @param doubleBufferIndex Index of the active buffer (0 or 1).
 * @param directProcess Indicates if the driver is in direct process mode.
 * @return User parameters.
 */
static void* asio_buffer_switch_time_info(void* params, long doubleBufferIndex,
                                          ASIOBool directProcess) {
  asio_buffer_switch(doubleBufferIndex, directProcess);
  return params;
}

static ASIOCallbacks asio_callbacks = {
    asio_buffer_switch, asio_sample_rate_did_change, asio_message,
    asio_buffer_switch_time_info};

// ==========================================
// Capture Backend Methods
// ==========================================

/**
 * @brief Internal method to open the ASIO capture stream.
 *
 * Initializes COM, creates and initializes the ASIO driver, sets sample rate,
 * allocates channel/buffer info arrays, creates buffers, and starts the driver
 * (if not in full-duplex mode where start is synchronized).
 *
 * @param ctx Pointer to the asio_capture_t context.
 * @param err Pointer to backend_error_t to receive error details.
 * @return true if successful, false otherwise.
 */
static bool asio_capture_open_internal(void* ctx, backend_error_t* err) {
  asio_capture_t* capture = (asio_capture_t*)ctx;
  HRESULT init_hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  capture->com_initialized = SUCCEEDED(init_hr);

  if (capture->full_duplex) {
    if (!register_and_wait_asio(
            true, capture->device, capture->sample_rate, capture->channels,
            capture->format, &capture->iasio, &capture->buffer_infos,
            &capture->channel_infos, &capture->actual_buffer_size, err)) {
      goto error_cleanup;
    }
  } else {
    CLSID clsid;
    if (!find_asio_driver_clsid(capture->device, &clsid)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_DEVICE_NOT_FOUND,
                           "ASIO capture driver not found");
      goto error_cleanup;
    }

    HRESULT hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &clsid,
                                  (void**)&capture->iasio);
    if (FAILED(hr)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to instantiate ASIO driver");
      goto error_cleanup;
    }

    if (!capture->iasio->lpVtbl->init(capture->iasio, GetDesktopWindow())) {
      SAFE_RELEASE(capture->iasio);
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to initialize ASIO driver");
      goto error_cleanup;
    }

    double rate = capture->sample_rate;
    double current_rate = 0.0;
    if (capture->iasio->lpVtbl->getSampleRate(capture->iasio, &current_rate) ==
        0) {
      if (fabs(current_rate - rate) > 0.5) {
        if (capture->iasio->lpVtbl->setSampleRate(capture->iasio, rate) == 0) {
          if (!force_sample_rate_with_dummy_cycle(capture->device,
                                                  &capture->iasio, rate, err)) {
            goto error_cleanup;
          }
        } else {
          if (err)
            backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                               "Failed to set ASIO sample rate");
          goto error_cleanup;
        }
      }
    }

    long min_sz, max_sz, pref_sz, granularity;
    capture->iasio->lpVtbl->getBufferSize(capture->iasio, &min_sz, &max_sz,
                                          &pref_sz, &granularity);
    capture->actual_buffer_size = pref_sz;

    int total_channels = capture->channels;
    capture->buffer_infos =
        (ASIOBufferInfo*)calloc(total_channels, sizeof(ASIOBufferInfo));
    capture->channel_infos =
        (ASIOChannelInfo*)calloc(total_channels, sizeof(ASIOChannelInfo));

    for (int i = 0; i < total_channels; i++) {
      capture->buffer_infos[i].isInput = ASIOTrue;
      capture->buffer_infos[i].channelNum = i;
      capture->channel_infos[i].channel = i;
      capture->channel_infos[i].isInput = ASIOTrue;
      capture->iasio->lpVtbl->getChannelInfo(capture->iasio,
                                             &capture->channel_infos[i]);
    }

    ASIOError create_buf_res = capture->iasio->lpVtbl->createBuffers(
        capture->iasio, capture->buffer_infos, total_channels,
        capture->actual_buffer_size, &asio_callbacks);
    if (create_buf_res != 0) {
      SAFE_RELEASE(capture->iasio);
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to create ASIO buffers");
      goto error_cleanup;
    }
  }

  capture->callback_buf_size = capture->actual_buffer_size * capture->channels;
  capture->callback_buf =
      (float*)malloc(capture->callback_buf_size * sizeof(float));
  if (!capture->callback_buf) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate ASIO callback buffer");
    goto error_cleanup;
  }

  size_t ring_size = capture->channels * capture->chunk_size * 8;
  capture->ring_buffer = spsc_audio_ring_buffer_create(ring_size);
  g_capture_event = CreateEvent(NULL, FALSE, FALSE, NULL);
  g_active_capture = capture;

  if (!capture->full_duplex) {
    ASIOError start_res = capture->iasio->lpVtbl->start(capture->iasio);
    if (start_res != 0) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to start ASIO driver");
      goto error_cleanup;
    }
  }

  capture->is_running = true;
  return true;

error_cleanup:
  asio_capture_close_internal(capture);
  return false;
}

/**
 * @brief Internal method to read samples from the ASIO capture stream.
 *
 * Consumes decoded float samples from the ring buffer and copies them to the
 * public audio_chunk_t structure. Blocks on g_capture_event if enough data is
 * not yet available.
 *
 * @param ctx Pointer to the asio_capture_t context.
 * @param frames Number of frames to read.
 * @param chunk Target audio chunk.
 * @param err Pointer to backend_error_t to receive error details.
 * @return true if successful, false otherwise.
 */
static bool asio_capture_read_internal(void* ctx, size_t frames,
                                       audio_chunk_t* chunk,
                                       backend_error_t* err) {
  (void)err;
  asio_capture_t* capture = (asio_capture_t*)ctx;

  size_t requested = frames * capture->channels;
  if (requested > capture->decode_buf_size) {
    float* new_buf =
        (float*)realloc(capture->decode_buf, requested * sizeof(float));
    if (!new_buf) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                           "Failed to reallocate ASIO capture decode buffer");
      return false;
    }
    capture->decode_buf = new_buf;
    capture->decode_buf_size = requested;
  }

  while (spsc_audio_ring_buffer_get_available_to_read(capture->ring_buffer) <
         requested) {
    if (WaitForSingleObject(g_capture_event, 100) != WAIT_OBJECT_0) {
      if (!capture->is_running) return false;
    }
  }

  size_t consumed = spsc_audio_ring_buffer_consume(
      capture->ring_buffer, capture->decode_buf, requested);
  if (consumed < requested) {
    memset(capture->decode_buf + consumed, 0,
           (requested - consumed) * sizeof(float));
  }

  for (size_t f = 0; f < frames; f++) {
    for (int c = 0; c < capture->channels; c++) {
      audio_chunk_get_channel(chunk, c)[f] =
          (double)capture->decode_buf[f * capture->channels + c];
    }
  }

  audio_chunk_set_valid_frames(chunk, frames);
  return true;
}

/**
 * @brief Internal method to close the ASIO capture stream.
 *
 * Stops and disposes of buffers (or releases shared ASIO if full-duplex),
 * and frees ring buffers, events, and COM.
 *
 * @param ctx Pointer to the asio_capture_t context.
 */
static void asio_capture_close_internal(void* ctx) {
  asio_capture_t* capture = (asio_capture_t*)ctx;
  if (!capture) return;
  if (capture->iasio) {
    capture->is_running = false;
    if (capture->full_duplex) {
      release_shared_asio(true, capture->iasio);
      capture->iasio = NULL;
    } else {
      capture->iasio->lpVtbl->stop(capture->iasio);
      capture->iasio->lpVtbl->disposeBuffers(capture->iasio);
      SAFE_RELEASE(capture->iasio);
    }
  }
  if (capture->ring_buffer) {
    spsc_audio_ring_buffer_free(capture->ring_buffer);
    capture->ring_buffer = NULL;
  }
  if (capture->decode_buf) {
    free(capture->decode_buf);
    capture->decode_buf = NULL;
    capture->decode_buf_size = 0;
  }
  if (capture->callback_buf) {
    free(capture->callback_buf);
    capture->callback_buf = NULL;
    capture->callback_buf_size = 0;
  }
  if (capture->buffer_infos) {
    free(capture->buffer_infos);
    capture->buffer_infos = NULL;
  }
  if (capture->channel_infos) {
    free(capture->channel_infos);
    capture->channel_infos = NULL;
  }
  if (g_capture_event) {
    CloseHandle(g_capture_event);
    g_capture_event = NULL;
  }
  if (g_active_capture == capture) g_active_capture = NULL;

  if (capture->com_initialized) {
    CoUninitialize();
    capture->com_initialized = false;
  }
}

/**
 * @brief Internal method to wait for capture data.
 *
 * Blocks on g_capture_event until the driver writes data to the ring buffer.
 *
 * @param ctx Pointer to the asio_capture_t context.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if data became available, false if timed out.
 */
static bool asio_capture_wait_for_data(void* ctx, uint32_t timeout_ms) {
  (void)ctx;
  return WaitForSingleObject(g_capture_event, timeout_ms) == WAIT_OBJECT_0;
}

/**
 * @brief Internal method to destroy the ASIO capture context.
 *
 * Closes the capture stream and frees context memory.
 *
 * @param ctx Pointer to the asio_capture_t context.
 */
static void asio_capture_destroy_internal(void* ctx) {
  asio_capture_close_internal(ctx);
  free(ctx);
}

static const capture_backend_vtable_t asio_capture_vtable = {
    .open = asio_capture_open_internal,
    .read = asio_capture_read_internal,
    .close = asio_capture_close_internal,
    .get_pending_rate_change = NULL,
    .is_pitch_control_supported = NULL,
    .set_pitch = NULL,
    .wait_for_data = asio_capture_wait_for_data,
    .set_is_paused = NULL,
    .destroy = asio_capture_destroy_internal};

capture_backend_t* asio_capture_new(const capture_device_config_t* config,
                                    int sample_rate, int chunk_size,
                                    bool full_duplex, backend_error_t* err) {
  (void)err;
  asio_capture_t* capture = (asio_capture_t*)calloc(1, sizeof(asio_capture_t));
  if (!capture) return NULL;

  snprintf(capture->device, sizeof(capture->device), "%s",
           config->cfg.asio.device);
  capture->channels = config->cfg.asio.channels;
  capture->sample_rate = sample_rate;
  capture->chunk_size = chunk_size;
  capture->format = config->cfg.asio.format;
  capture->full_duplex = full_duplex;

  capture_backend_t* backend =
      (capture_backend_t*)malloc(sizeof(capture_backend_t));
  if (!backend) {
    free(capture);
    return NULL;
  }
  backend->ctx = capture;
  backend->vtable = &asio_capture_vtable;
  return backend;
}

// ==========================================
// Playback Backend Methods
// ==========================================

/**
 * @brief Internal method to open the ASIO playback stream.
 *
 * @param ctx Pointer to the asio_playback_t context.
 * @param err Pointer to backend_error_t to receive error details.
 * @return true if successful, false otherwise.
 */
static bool asio_playback_open_internal(void* ctx, backend_error_t* err) {
  asio_playback_t* playback = (asio_playback_t*)ctx;
  HRESULT init_hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  playback->com_initialized = SUCCEEDED(init_hr);

  if (playback->full_duplex) {
    if (!register_and_wait_asio(
            false, playback->device, playback->sample_rate, playback->channels,
            playback->format, &playback->iasio, &playback->buffer_infos,
            &playback->channel_infos, &playback->actual_buffer_size, err)) {
      goto error_cleanup;
    }
  } else {
    CLSID clsid;
    if (!find_asio_driver_clsid(playback->device, &clsid)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_DEVICE_NOT_FOUND,
                           "ASIO playback driver not found");
      goto error_cleanup;
    }

    HRESULT hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &clsid,
                                  (void**)&playback->iasio);
    if (FAILED(hr)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to instantiate ASIO driver");
      goto error_cleanup;
    }

    if (!playback->iasio->lpVtbl->init(playback->iasio, GetDesktopWindow())) {
      SAFE_RELEASE(playback->iasio);
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to initialize ASIO driver");
      goto error_cleanup;
    }

    double rate = playback->sample_rate;
    double current_rate = 0.0;
    if (playback->iasio->lpVtbl->getSampleRate(playback->iasio,
                                               &current_rate) == 0) {
      if (fabs(current_rate - rate) > 0.5) {
        if (playback->iasio->lpVtbl->setSampleRate(playback->iasio, rate) ==
            0) {
          if (!force_sample_rate_with_dummy_cycle(
                  playback->device, &playback->iasio, rate, err)) {
            goto error_cleanup;
          }
        } else {
          if (err)
            backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                               "Failed to set ASIO sample rate");
          goto error_cleanup;
        }
      }
    }

    long min_sz, max_sz, pref_sz, granularity;
    playback->iasio->lpVtbl->getBufferSize(playback->iasio, &min_sz, &max_sz,
                                           &pref_sz, &granularity);
    playback->actual_buffer_size = pref_sz;

    long num_in = 0, num_out = 0;
    playback->iasio->lpVtbl->getChannels(playback->iasio, &num_in, &num_out);

    int total_allocated = num_in + playback->channels;
    playback->buffer_infos =
        (ASIOBufferInfo*)calloc(total_allocated, sizeof(ASIOBufferInfo));
    playback->channel_infos =
        (ASIOChannelInfo*)calloc(total_allocated, sizeof(ASIOChannelInfo));

    for (int i = 0; i < num_in; i++) {
      playback->buffer_infos[i].isInput = ASIOTrue;
      playback->buffer_infos[i].channelNum = i;
    }

    for (int i = 0; i < playback->channels; i++) {
      int idx = num_in + i;
      playback->buffer_infos[idx].isInput = ASIOFalse;
      playback->buffer_infos[idx].channelNum = i;
      playback->channel_infos[idx].channel = i;
      playback->channel_infos[idx].isInput = ASIOFalse;
      playback->iasio->lpVtbl->getChannelInfo(playback->iasio,
                                              &playback->channel_infos[idx]);
    }

    ASIOError create_buf_res = playback->iasio->lpVtbl->createBuffers(
        playback->iasio, playback->buffer_infos, total_allocated,
        playback->actual_buffer_size, &asio_callbacks);
    if (create_buf_res != 0) {
      SAFE_RELEASE(playback->iasio);
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to create ASIO buffers");
      goto error_cleanup;
    }
  }

  playback->callback_buf_size =
      playback->actual_buffer_size * playback->channels;
  playback->callback_buf =
      (float*)malloc(playback->callback_buf_size * sizeof(float));
  if (!playback->callback_buf) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate ASIO callback buffer");
    goto error_cleanup;
  }

  size_t ring_size = playback->channels * playback->chunk_size * 8;
  playback->ring_buffer = spsc_audio_ring_buffer_create(ring_size);
  g_active_playback = playback;

  if (!playback->full_duplex) {
    ASIOError start_res = playback->iasio->lpVtbl->start(playback->iasio);
    if (start_res != 0) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to start ASIO driver");
      goto error_cleanup;
    }
  }

  playback->is_running = true;
  return true;

error_cleanup:
  asio_playback_close_internal(playback);
  return false;
}

/**
 * @brief Internal method to write samples to the ASIO playback stream.
 *
 * Encodes double format samples to flat float array, and writes to the playback
 * ring buffer. Blocks (sleeps) if the ring buffer does not have enough
 * capacity.
 *
 * @param ctx Pointer to the asio_playback_t context.
 * @param chunk Audio chunk to write.
 * @param err Pointer to backend_error_t to receive error details.
 * @return true if successful, false otherwise.
 */
static bool asio_playback_write_internal(void* ctx, const audio_chunk_t* chunk,
                                         backend_error_t* err) {
  (void)err;
  asio_playback_t* playback = (asio_playback_t*)ctx;

  size_t requested = audio_chunk_get_valid_frames(chunk) * playback->channels;
  if (requested > playback->encode_buf_size) {
    float* new_buf =
        (float*)realloc(playback->encode_buf, requested * sizeof(float));
    if (!new_buf) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "Failed to reallocate ASIO playback encode buffer");
      return false;
    }
    playback->encode_buf = new_buf;
    playback->encode_buf_size = requested;
  }

  for (size_t f = 0; f < audio_chunk_get_valid_frames(chunk); f++) {
    for (int c = 0; c < playback->channels; c++) {
      playback->encode_buf[f * playback->channels + c] =
          (float)audio_chunk_get_channel(chunk, c)[f];
    }
  }

  size_t written = 0;
  while (written < requested) {
    // Since ring buffer holds flat interleaved float array, stride = 1
    size_t available_space =
        spsc_audio_ring_buffer_get_capacity(playback->ring_buffer) -
        spsc_audio_ring_buffer_get_available_to_read(playback->ring_buffer);
    size_t to_write = requested - written;
    if (to_write > available_space) to_write = available_space;

    if (to_write > 0) {
      spsc_audio_ring_buffer_write(playback->ring_buffer,
                                   playback->encode_buf + written, to_write, 1);
      written += to_write;
    } else {
      Sleep(1);
      if (!playback->is_running) return false;
    }
  }
  return true;
}

/**
 * @brief Internal method to close the ASIO playback stream.
 *
 * @param ctx Pointer to the asio_playback_t context.
 */
static void asio_playback_close_internal(void* ctx) {
  asio_playback_t* playback = (asio_playback_t*)ctx;
  if (!playback) return;
  if (playback->iasio) {
    playback->is_running = false;
    if (playback->full_duplex) {
      release_shared_asio(false, playback->iasio);
      playback->iasio = NULL;
    } else {
      playback->iasio->lpVtbl->stop(playback->iasio);
      playback->iasio->lpVtbl->disposeBuffers(playback->iasio);
      SAFE_RELEASE(playback->iasio);
    }
  }
  if (playback->ring_buffer) {
    spsc_audio_ring_buffer_free(playback->ring_buffer);
    playback->ring_buffer = NULL;
  }
  if (playback->encode_buf) {
    free(playback->encode_buf);
    playback->encode_buf = NULL;
    playback->encode_buf_size = 0;
  }
  if (playback->callback_buf) {
    free(playback->callback_buf);
    playback->callback_buf = NULL;
    playback->callback_buf_size = 0;
  }
  if (playback->buffer_infos) {
    free(playback->buffer_infos);
    playback->buffer_infos = NULL;
  }
  if (playback->channel_infos) {
    free(playback->channel_infos);
    playback->channel_infos = NULL;
  }
  if (g_active_playback == playback) g_active_playback = NULL;

  if (playback->com_initialized) {
    CoUninitialize();
    playback->com_initialized = false;
  }
}

/**
 * @brief Internal method to get the current buffer level of the playback
 * stream.
 *
 * Returns the amount of unconsumed audio frames currently in the ring buffer.
 *
 * @param ctx Pointer to the asio_playback_t context.
 * @return Number of frames.
 */
static size_t asio_playback_get_buffer_level(void* ctx) {
  asio_playback_t* playback = (asio_playback_t*)ctx;
  return playback->ring_buffer ? (spsc_audio_ring_buffer_get_available_to_read(
                                      playback->ring_buffer) /
                                  playback->channels)
                               : 0;
}

/**
 * @brief Internal method to destroy the ASIO playback context.
 *
 * @param ctx Pointer to the asio_playback_t context.
 */
static void asio_playback_destroy_internal(void* ctx) {
  asio_playback_close_internal(ctx);
  free(ctx);
}

static const playback_backend_vtable_t asio_playback_vtable = {
    .open = asio_playback_open_internal,
    .write = asio_playback_write_internal,
    .close = asio_playback_close_internal,
    .get_buffer_level = asio_playback_get_buffer_level,
    .get_pending_rate_change = NULL,
    .prefill_silence = NULL,
    .get_is_paused = NULL,
    .set_is_paused = NULL,
    .pitch_control_supported = NULL,
    .set_pitch = NULL,
    .destroy = asio_playback_destroy_internal};

playback_backend_t* asio_playback_new(const playback_device_config_t* config,
                                      int sample_rate, int chunk_size,
                                      bool full_duplex, backend_error_t* err) {
  (void)err;
  asio_playback_t* playback =
      (asio_playback_t*)calloc(1, sizeof(asio_playback_t));
  if (!playback) return NULL;

  snprintf(playback->device, sizeof(playback->device), "%s",
           config->cfg.asio.device);
  playback->channels = config->cfg.asio.channels;
  playback->sample_rate = sample_rate;
  playback->chunk_size = chunk_size;
  playback->format = config->cfg.asio.format;
  playback->full_duplex = full_duplex;

  playback_backend_t* backend =
      (playback_backend_t*)malloc(sizeof(playback_backend_t));
  if (!backend) {
    free(playback);
    return NULL;
  }
  backend->ctx = playback;
  backend->vtable = &asio_playback_vtable;
  return backend;
}

#endif  // ENABLE_ASIO
