#if defined(ENABLE_ASIO)

#define WIN32_LEAN_AND_MEAN
#include "asio_capabilities.h"

#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unknwn.h>
#include <windows.h>

#include "Logging/app_logger.h"

// COM Release helper
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

// Forward declaration of COM interface
typedef struct IASIO IASIO;
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

/**
 * @brief Searches the Windows Registry to find the CLSID of an ASIO driver by
 * name.
 *
 * @param driver_name Name of the ASIO driver.
 * @param out_clsid Pointer to a CLSID structure to receive the result.
 * @return true if the CLSID was successfully found, false otherwise.
 */
static bool find_asio_driver_caps_clsid(const char* driver_name,
                                        CLSID* out_clsid) {
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

int asio_capabilities_available_device_names(bool is_capture,
                                             char out_names[][256],
                                             int max_names) {
  (void)is_capture;
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

  HKEY hk;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\ASIO", 0, KEY_READ, &hk) !=
      ERROR_SUCCESS) {
    CoUninitialize();
    return 0;
  }

  char subkey_name[256];
  DWORD index = 0;
  int matched = 0;

  while (RegEnumKeyA(hk, index++, subkey_name, sizeof(subkey_name)) ==
             ERROR_SUCCESS &&
         matched < max_names) {
    snprintf(out_names[matched++], 256, "%s", subkey_name);
  }
  RegCloseKey(hk);
  CoUninitialize();
  return matched;
}

bool asio_capabilities_default_device_name(bool is_capture, char* out_name,
                                           size_t max_len) {
  (void)is_capture;
  char names[1][256];
  int count = asio_capabilities_available_device_names(is_capture, names, 1);
  if (count > 0) {
    snprintf(out_name, max_len, "%s", names[0]);
    return true;
  }
  out_name[0] = '\0';
  return false;
}

audio_device_descriptor_t* asio_capabilities_describe(const char* device_name,
                                                      bool is_capture,
                                                      device_error_t* err) {
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

  CLSID clsid;
  if (!find_asio_driver_caps_clsid(device_name, &clsid)) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_NOT_FOUND, "ASIO driver not found");
    }
    goto error_cleanup;
  }

  IASIO* iasio = NULL;
  HRESULT hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &clsid,
                                (void**)&iasio);
  if (FAILED(hr)) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER,
                        "Failed to instantiate ASIO COM object");
    }
    goto error_cleanup;
  }

  if (!iasio->lpVtbl->init(iasio, GetDesktopWindow())) {
    if (err) {
      device_error_init(
          err, DEVICE_ERROR_BUSY,
          "ASIO driver initialization failed (busy or disconnected)");
    }
    SAFE_RELEASE(iasio);
    goto error_cleanup;
  }

  long num_inputs = 0, num_outputs = 0;
  iasio->lpVtbl->getChannels(iasio, &num_inputs, &num_outputs);
  long target_channels = is_capture ? num_inputs : num_outputs;
  if (target_channels <= 0) {
    if (err) {
      device_error_init(
          err, DEVICE_ERROR_OTHER,
          "ASIO driver has no channels in the requested direction");
    }
    SAFE_RELEASE(iasio);
    goto error_cleanup;
  }

  audio_device_descriptor_t* desc =
      (audio_device_descriptor_t*)calloc(1, sizeof(audio_device_descriptor_t));
  if (!desc) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER, "Out of memory");
    }
    SAFE_RELEASE(iasio);
    goto error_cleanup;
  }
  snprintf(desc->name, sizeof(desc->name), "%s", device_name);

  desc->capability_sets_count = 1;
  desc->capability_sets =
      (device_capability_set_t*)calloc(1, sizeof(device_capability_set_t));
  if (!desc->capability_sets) {
    free_audio_device_descriptor(desc);
    SAFE_RELEASE(iasio);
    goto error_cleanup;
  }

  device_capability_set_t* set = &desc->capability_sets[0];
  set->capabilities_count = 1;
  set->capabilities =
      (channel_capability_t*)calloc(1, sizeof(channel_capability_t));
  if (!set->capabilities) {
    free_audio_device_descriptor(desc);
    SAFE_RELEASE(iasio);
    goto error_cleanup;
  }

  channel_capability_t* cap = &set->capabilities[0];
  cap->channels = (int)target_channels;

  // Probe supported sample rates
  // Probe supported sample rates from a predefined list.
  // We use the ASIO driver's canSampleRate method to check each rate.
  const double PROBE_RATES[] = {
      5512.0,   8000.0,   11025.0,  16000.0,  22050.0, 32000.0,
      44100.0,  48000.0,  64000.0,  88200.0,  96000.0, 176400.0,
      192000.0, 352800.0, 384000.0, 705600.0, 768000.0};
  const size_t PROBE_RATES_COUNT = sizeof(PROBE_RATES) / sizeof(PROBE_RATES[0]);

  cap->samplerates = (samplerate_capability_t*)calloc(
      PROBE_RATES_COUNT, sizeof(samplerate_capability_t));
  if (!cap->samplerates) {
    free_audio_device_descriptor(desc);
    SAFE_RELEASE(iasio);
    goto error_cleanup;
  }
  size_t valid_rates_count = 0;

  // Probe native sample format of the first channel.
  // ASIO usually expects all channels to share the same sample type.
  ASIOChannelInfo chan_info;
  memset(&chan_info, 0, sizeof(chan_info));
  chan_info.channel = 0;
  chan_info.isInput = is_capture ? ASIOTrue : ASIOFalse;
  iasio->lpVtbl->getChannelInfo(iasio, &chan_info);

  // Map the ASIO-specific sample format to our internal format string
  // representation.
  const char* native_fmt_name = "S32_LE";  // fallback
  if (chan_info.type == ASIOSTInt16LSB)
    native_fmt_name = "S16_LE";
  else if (chan_info.type == ASIOSTInt24LSB)
    native_fmt_name = "S24_3_LE";
  else if (chan_info.type == ASIOSTInt32LSB ||
           chan_info.type == ASIOSTInt32LSB16 ||
           chan_info.type == ASIOSTInt32LSB18 ||
           chan_info.type == ASIOSTInt32LSB20)
    native_fmt_name = "S32_LE";
  else if (chan_info.type == ASIOSTInt32LSB24)
    native_fmt_name = "S24_4_LE";
  else if (chan_info.type == ASIOSTFloat32LSB)
    native_fmt_name = "F32_LE";
  else if (chan_info.type == ASIOSTFloat64LSB)
    native_fmt_name = "F64_LE";

  for (size_t r = 0; r < PROBE_RATES_COUNT; r++) {
    double rate = PROBE_RATES[r];
    if (iasio->lpVtbl->canSampleRate(iasio, rate) == 0) {  // ASE_OK is 0
      samplerate_capability_t* rate_cap =
          &cap->samplerates[valid_rates_count++];
      rate_cap->samplerate = (int)rate;
      rate_cap->formats = (char**)calloc(1, sizeof(char*));
      if (!rate_cap->formats) {
        free_audio_device_descriptor(desc);
        SAFE_RELEASE(iasio);
        goto error_cleanup;
      }
      rate_cap->formats[0] = strdup(native_fmt_name);
      if (!rate_cap->formats[0]) {
        free_audio_device_descriptor(desc);
        SAFE_RELEASE(iasio);
        goto error_cleanup;
      }
      rate_cap->formats_count = 1;
    }
  }

  cap->samplerates_count = valid_rates_count;
  if (valid_rates_count == 0) {
    free_audio_device_descriptor(desc);
    desc = NULL;
  }

  SAFE_RELEASE(iasio);
  CoUninitialize();
  return desc;

error_cleanup:
  if (desc) {
    free_audio_device_descriptor(desc);
  }
  CoUninitialize();
  return NULL;
}

#endif  // ENABLE_ASIO
