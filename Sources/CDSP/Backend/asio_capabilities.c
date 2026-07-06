#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include "asio_capabilities.h"

#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unknwn.h>
#include <windows.h>

#include "Logging/app_logger.h"

// IASIO Interface GUID
// {3F12C5C4-4850-11d1-89E2-0000E819C656}
DEFINE_GUID(IID_IASIO_CAPS, 0x3f12c5c4, 0x4850, 0x11d1, 0x89, 0xe2, 0x00, 0x00,
            0xe8, 0x19, 0xc6, 0x56);

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

typedef enum {
  ASIOSTInt16MSB = 0,
  ASIOSTInt24MSB = 1,
  ASIOSTInt32MSB = 2,
  ASIOSTFloat32MSB = 3,
  ASIOSTFloat64MSB = 4,
  ASIOSTInt16LSB = 16,
  ASIOSTInt24LSB = 17,
  ASIOSTInt32LSB = 18,
  ASIOSTFloat32LSB = 19,
  ASIOSTFloat64LSB = 20,
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
  HRESULT(STDMETHODCALLTYPE* getDriverName)(IASIO* This, char* name);
  long(STDMETHODCALLTYPE* getDriverVersion)(IASIO* This);
  HRESULT(STDMETHODCALLTYPE* getErrorMessage)(IASIO* This, char* string);
  HRESULT(STDMETHODCALLTYPE* start)(IASIO* This);
  HRESULT(STDMETHODCALLTYPE* stop)(IASIO* This);
  HRESULT(STDMETHODCALLTYPE* getChannels)(IASIO* This, long* numInputChannels,
                                          long* numOutputChannels);
  HRESULT(STDMETHODCALLTYPE* getLatencies)(IASIO* This, long* inputLatency,
                                           long* outputLatency);
  HRESULT(STDMETHODCALLTYPE* getBufferSize)(IASIO* This, long* minSize,
                                            long* maxSize, long* preferredSize,
                                            long* granularity);
  HRESULT(STDMETHODCALLTYPE* canSampleRate)(IASIO* This, double sampleRate);
  HRESULT(STDMETHODCALLTYPE* getSampleRate)(IASIO* This, double* sampleRate);
  HRESULT(STDMETHODCALLTYPE* setSampleRate)(IASIO* This, double sampleRate);
  HRESULT(STDMETHODCALLTYPE* getClockSources)(IASIO* This, void* clocks,
                                              long* numSources);
  HRESULT(STDMETHODCALLTYPE* setClockSource)(IASIO* This, long reference);
  HRESULT(STDMETHODCALLTYPE* getSamplePosition)(IASIO* This, int64_t* sPos,
                                                int64_t* tStamp);
  HRESULT(STDMETHODCALLTYPE* getChannelInfo)(IASIO* This, void* info);
  HRESULT(STDMETHODCALLTYPE* createBuffers)(IASIO* This, void* bufferInfos,
                                            long numChannels, long bufferSize,
                                            void* callbacks);
  HRESULT(STDMETHODCALLTYPE* disposeBuffers)(IASIO* This);
  HRESULT(STDMETHODCALLTYPE* controlPanel)(IASIO* This);
  HRESULT(STDMETHODCALLTYPE* future)(IASIO* This, long selector, void* opt);
  HRESULT(STDMETHODCALLTYPE* outputReady)(IASIO* This);
} IASIOVtbl;

struct IASIO {
  const IASIOVtbl* lpVtbl;
};

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
  CoInitializeEx(NULL, COINIT_MULTITHREADED);

  HKEY hk;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\ASIO", 0, KEY_READ, &hk) !=
      ERROR_SUCCESS) {
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
                                                      bool is_capture) {
  CoInitializeEx(NULL, COINIT_MULTITHREADED);

  CLSID clsid;
  if (!find_asio_driver_caps_clsid(device_name, &clsid)) {
    return NULL;
  }

  IASIO* iasio = NULL;
  HRESULT hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IASIO_CAPS, (void**)&iasio);
  if (FAILED(hr)) return NULL;

  if (!iasio->lpVtbl->init(iasio, GetDesktopWindow())) {
    SAFE_RELEASE(iasio);
    return NULL;
  }

  long num_inputs = 0, num_outputs = 0;
  iasio->lpVtbl->getChannels(iasio, &num_inputs, &num_outputs);
  long target_channels = is_capture ? num_inputs : num_outputs;
  if (target_channels <= 0) {
    SAFE_RELEASE(iasio);
    return NULL;
  }

  audio_device_descriptor_t* desc =
      (audio_device_descriptor_t*)calloc(1, sizeof(audio_device_descriptor_t));
  if (!desc) {
    SAFE_RELEASE(iasio);
    return NULL;
  }
  snprintf(desc->name, sizeof(desc->name), "%s", device_name);

  desc->capability_sets_count = 1;
  desc->capability_sets =
      (device_capability_set_t*)calloc(1, sizeof(device_capability_set_t));

  device_capability_set_t* set = &desc->capability_sets[0];
  set->capabilities_count = 1;
  set->capabilities =
      (channel_capability_t*)calloc(1, sizeof(channel_capability_t));

  channel_capability_t* cap = &set->capabilities[0];
  cap->channels = (int)target_channels;

  // Probe supported sample rates
  const double PROBE_RATES[] = {44100.0, 48000.0,  88200.0,
                                96000.0, 176400.0, 192000.0};
  const size_t PROBE_RATES_COUNT = sizeof(PROBE_RATES) / sizeof(PROBE_RATES[0]);

  cap->samplerates = (samplerate_capability_t*)calloc(
      PROBE_RATES_COUNT, sizeof(samplerate_capability_t));
  size_t valid_rates_count = 0;

  // Probe format of first channel
  ASIOChannelInfo chan_info;
  memset(&chan_info, 0, sizeof(chan_info));
  chan_info.channel = 0;
  chan_info.isInput = is_capture ? ASIOTrue : ASIOFalse;
  iasio->lpVtbl->getChannelInfo(iasio, &chan_info);

  const char* native_fmt_name = "S32_LE";  // fallback
  if (chan_info.type == ASIOSTInt16LSB)
    native_fmt_name = "S16_LE";
  else if (chan_info.type == ASIOSTInt24LSB)
    native_fmt_name = "S24_3_LE";
  else if (chan_info.type == ASIOSTInt32LSB)
    native_fmt_name = "S32_LE";
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
      rate_cap->formats_count = 1;
      rate_cap->formats = (char**)calloc(1, sizeof(char*));
      rate_cap->formats[0] = strdup(native_fmt_name);
    }
  }

  cap->samplerates_count = valid_rates_count;
  if (valid_rates_count == 0) {
    free(cap->samplerates);
    free(set->capabilities);
    free(desc->capability_sets);
    free(desc);
    desc = NULL;
  }

  SAFE_RELEASE(iasio);
  return desc;
}

void asio_capabilities_free_descriptor(audio_device_descriptor_t* desc) {
  if (!desc) return;
  if (desc->capability_sets) {
    for (size_t s = 0; s < desc->capability_sets_count; s++) {
      device_capability_set_t* set = &desc->capability_sets[s];
      if (set->capabilities) {
        for (size_t c = 0; c < set->capabilities_count; c++) {
          channel_capability_t* ch_cap = &set->capabilities[c];
          if (ch_cap->samplerates) {
            for (size_t r = 0; r < ch_cap->samplerates_count; r++) {
              samplerate_capability_t* rate_cap = &ch_cap->samplerates[r];
              if (rate_cap->formats) {
                for (size_t f = 0; f < rate_cap->formats_count; f++) {
                  free(rate_cap->formats[f]);
                }
                free(rate_cap->formats);
              }
            }
            free(ch_cap->samplerates);
          }
        }
        free(set->capabilities);
      }
    }
    free(desc->capability_sets);
  }
  free(desc);
}

#endif  // _WIN32
