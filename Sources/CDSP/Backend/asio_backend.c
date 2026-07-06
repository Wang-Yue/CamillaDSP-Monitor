#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <unknwn.h>
#include "asio_backend.h"
#include "Audio/lock_free_ring_buffer.h"
#include "Logging/app_logger.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// IASIO Interface GUID
// {3F12C5C4-4850-11d1-89E2-0000E819C656}
DEFINE_GUID(IID_IASIO, 0x3f12c5c4, 0x4850, 0x11d1, 0x89, 0xe2, 0x00, 0x00, 0xe8, 0x19, 0xc6, 0x56);

// COM Release helper
#define SAFE_RELEASE(punk) \
    if ((punk) != NULL) { \
        (punk)->lpVtbl->Release(punk); \
        (punk) = NULL; \
    }

// ASIO type definitions
typedef int32_t ASIOBool;
#define ASIOFalse 0
#define ASIOTrue 1

typedef double ASIOSampleRate;

typedef enum {
    ASIOSTInt16MSB   = 0,
    ASIOSTInt24MSB   = 1,
    ASIOSTInt32MSB   = 2,
    ASIOSTFloat32MSB = 3,
    ASIOSTFloat64MSB = 4,
    ASIOSTInt16LSB   = 16,
    ASIOSTInt24LSB   = 17,
    ASIOSTInt32LSB   = 18,
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

typedef struct {
    ASIOBool isInput;
    int32_t channelNum;
    void* buffers[2];
} ASIOBufferInfo;

typedef struct {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    void* (*bufferSwitchTimeInfo)(void* params, long doubleBufferIndex, ASIOBool directProcess);
} ASIOCallbacks;

// Forward declaration of COM interface
typedef struct IASIO IASIO;
typedef struct IASIOVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IASIO* This, REFIID riid, void** ppvObject);
    ULONG (STDMETHODCALLTYPE *AddRef)(IASIO* This);
    ULONG (STDMETHODCALLTYPE *Release)(IASIO* This);
    ASIOBool (STDMETHODCALLTYPE *init)(IASIO* This, void* sysHandle);
    HRESULT (STDMETHODCALLTYPE *getDriverName)(IASIO* This, char* name);
    long (STDMETHODCALLTYPE *getDriverVersion)(IASIO* This);
    HRESULT (STDMETHODCALLTYPE *getErrorMessage)(IASIO* This, char* string);
    HRESULT (STDMETHODCALLTYPE *start)(IASIO* This);
    HRESULT (STDMETHODCALLTYPE *stop)(IASIO* This);
    HRESULT (STDMETHODCALLTYPE *getChannels)(IASIO* This, long* numInputChannels, long* numOutputChannels);
    HRESULT (STDMETHODCALLTYPE *getLatencies)(IASIO* This, long* inputLatency, long* outputLatency);
    HRESULT (STDMETHODCALLTYPE *getBufferSize)(IASIO* This, long* minSize, long* maxSize, long* preferredSize, long* granularity);
    HRESULT (STDMETHODCALLTYPE *canSampleRate)(IASIO* This, double sampleRate);
    HRESULT (STDMETHODCALLTYPE *getSampleRate)(IASIO* This, double* sampleRate);
    HRESULT (STDMETHODCALLTYPE *setSampleRate)(IASIO* This, double sampleRate);
    HRESULT (STDMETHODCALLTYPE *getClockSources)(IASIO* This, void* clocks, long* numSources);
    HRESULT (STDMETHODCALLTYPE *setClockSource)(IASIO* This, long reference);
    HRESULT (STDMETHODCALLTYPE *getSamplePosition)(IASIO* This, int64_t* sPos, int64_t* tStamp);
    HRESULT (STDMETHODCALLTYPE *getChannelInfo)(IASIO* This, void* info);
    HRESULT (STDMETHODCALLTYPE *createBuffers)(IASIO* This, void* bufferInfos, long numChannels, long bufferSize, void* callbacks);
    HRESULT (STDMETHODCALLTYPE *disposeBuffers)(IASIO* This);
    HRESULT (STDMETHODCALLTYPE *controlPanel)(IASIO* This);
    HRESULT (STDMETHODCALLTYPE *future)(IASIO* This, long selector, void* opt);
    HRESULT (STDMETHODCALLTYPE *outputReady)(IASIO* This);
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
};

// Global active backend references
static asio_capture_t* g_active_capture = NULL;
static asio_playback_t* g_active_playback = NULL;
static HANDLE g_capture_event = NULL;

static bool find_asio_driver_clsid(const char* driver_name, CLSID* out_clsid) {
    HKEY hk;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\ASIO", 0, KEY_READ, &hk) != ERROR_SUCCESS) {
        return false;
    }
    
    char subkey_name[256];
    DWORD index = 0;
    bool found = false;
    
    while (RegEnumKeyA(hk, index++, subkey_name, sizeof(subkey_name)) == ERROR_SUCCESS) {
        if (driver_name[0] == '\0' || strcasecmp(subkey_name, driver_name) == 0 || strstr(subkey_name, driver_name) != NULL) {
            HKEY hk_driver;
            if (RegOpenKeyExA(hk, subkey_name, 0, KEY_READ, &hk_driver) == ERROR_SUCCESS) {
                char clsid_str[128];
                DWORD size = sizeof(clsid_str);
                if (RegQueryValueExA(hk_driver, "CLSID", NULL, NULL, (LPBYTE)clsid_str, &size) == ERROR_SUCCESS) {
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
static void asio_buffer_switch(long doubleBufferIndex, ASIOBool directProcess) {
    (void)directProcess;
    
    // Capture phase
    if (g_active_capture && g_active_capture->is_running) {
        long frames = g_active_capture->actual_buffer_size;
        int channels = g_active_capture->channels;
        
        float* interleaved_buf = (float*)malloc(frames * channels * sizeof(float));
        if (interleaved_buf) {
            for (int c = 0; c < channels; c++) {
                void* src = g_active_capture->buffer_infos[c].buffers[doubleBufferIndex];
                int type = g_active_capture->channel_infos[c].type;
                
                for (long f = 0; f < frames; f++) {
                    float val = 0.0f;
                    if (type == ASIOSTInt16LSB) {
                        val = ((int16_t*)src)[f] / 32768.0f;
                    } else if (type == ASIOSTInt32LSB) {
                        val = ((int32_t*)src)[f] / 2147483648.0f;
                    } else if (type == ASIOSTFloat32LSB) {
                        val = ((float*)src)[f];
                    } else if (type == ASIOSTInt24LSB) {
                        uint8_t* p = &((uint8_t*)src)[f * 3];
                        int32_t ival = (p[0]) | (p[1] << 8) | (p[2] << 16);
                        if (ival & 0x800000) ival |= 0xFF000000;
                        val = ival / 8388608.0f;
                    }
                    interleaved_buf[f * channels + c] = val;
                }
            }
            
            spsc_audio_ring_buffer_write(g_active_capture->ring_buffer, interleaved_buf, frames * channels, 1);
            free(interleaved_buf);
            if (g_capture_event) SetEvent(g_capture_event);
        }
    }
    
    // Playback phase
    if (g_active_playback && g_active_playback->is_running) {
        long frames = g_active_playback->actual_buffer_size;
        int channels = g_active_playback->channels;
        
        float* interleaved_buf = (float*)malloc(frames * channels * sizeof(float));
        if (interleaved_buf) {
            size_t read_samples = spsc_audio_ring_buffer_consume(g_active_playback->ring_buffer, interleaved_buf, frames * channels);
            if (read_samples < (size_t)(frames * channels)) {
                memset(interleaved_buf + read_samples, 0, (frames * channels - read_samples) * sizeof(float));
            }
            
            long num_in = 0, num_out = 0;
            g_active_playback->iasio->lpVtbl->getChannels(g_active_playback->iasio, &num_in, &num_out);
            
            for (int c = 0; c < channels; c++) {
                int buf_idx = num_in + c;
                void* dst = g_active_playback->buffer_infos[buf_idx].buffers[doubleBufferIndex];
                int type = g_active_playback->channel_infos[buf_idx].type;
                
                for (long f = 0; f < frames; f++) {
                    float val = interleaved_buf[f * channels + c];
                    if (val > 1.0f) val = 1.0f;
                    else if (val < -1.0f) val = -1.0f;
                    
                    if (type == ASIOSTInt16LSB) {
                        ((int16_t*)dst)[f] = (int16_t)(val * 32767.0f);
                    } else if (type == ASIOSTInt32LSB) {
                        ((int32_t*)dst)[f] = (int32_t)(val * 2147483647.0f);
                    } else if (type == ASIOSTFloat32LSB) {
                        ((float*)dst)[f] = val;
                    } else if (type == ASIOSTInt24LSB) {
                        int32_t ival = (int32_t)(val * 8388607.0f);
                        uint8_t* p = &((uint8_t*)dst)[f * 3];
                        p[0] = ival & 0xFF;
                        p[1] = (ival >> 8) & 0xFF;
                        p[2] = (ival >> 16) & 0xFF;
                    }
                }
            }
            free(interleaved_buf);
        }
    }
}

static void asio_sample_rate_did_change(ASIOSampleRate sRate) {
    (void)sRate;
}

static long asio_message(long selector, long value, void* message, double* opt) {
    (void)selector; (void)value; (void)message; (void)opt;
    return 0;
}

static void* asio_buffer_switch_time_info(void* params, long doubleBufferIndex, ASIOBool directProcess) {
    asio_buffer_switch(doubleBufferIndex, directProcess);
    return params;
}

static ASIOCallbacks asio_callbacks = {
    asio_buffer_switch,
    asio_sample_rate_did_change,
    asio_message,
    asio_buffer_switch_time_info
};

// ==========================================
// Capture Backend Methods
// ==========================================

static bool asio_capture_open_internal(void* ctx, backend_error_t* err) {
    asio_capture_t* capture = (asio_capture_t*)ctx;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    
    CLSID clsid;
    if (!find_asio_driver_clsid(capture->device, &clsid)) {
        if (err) backend_error_init(err, BACKEND_ERROR_DEVICE_NOT_FOUND, "ASIO capture driver not found");
        return false;
    }
    
    HRESULT hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IASIO, (void**)&capture->iasio);
    if (FAILED(hr)) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to instantiate ASIO driver");
        return false;
    }
    
    if (!capture->iasio->lpVtbl->init(capture->iasio, GetDesktopWindow())) {
        SAFE_RELEASE(capture->iasio);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to initialize ASIO driver");
        return false;
    }
    
    hr = capture->iasio->lpVtbl->setSampleRate(capture->iasio, capture->sample_rate);
    if (FAILED(hr)) {
        SAFE_RELEASE(capture->iasio);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "ASIO sample rate not supported");
        return false;
    }
    
    long min_sz, max_sz, pref_sz, granularity;
    capture->iasio->lpVtbl->getBufferSize(capture->iasio, &min_sz, &max_sz, &pref_sz, &granularity);
    capture->actual_buffer_size = pref_sz;
    
    int total_channels = capture->channels;
    capture->buffer_infos = (ASIOBufferInfo*)calloc(total_channels, sizeof(ASIOBufferInfo));
    capture->channel_infos = (ASIOChannelInfo*)calloc(total_channels, sizeof(ASIOChannelInfo));
    
    for (int i = 0; i < total_channels; i++) {
        capture->buffer_infos[i].isInput = ASIOTrue;
        capture->buffer_infos[i].channelNum = i;
        capture->channel_infos[i].channel = i;
        capture->channel_infos[i].isInput = ASIOTrue;
        capture->iasio->lpVtbl->getChannelInfo(capture->iasio, &capture->channel_infos[i]);
    }
    
    hr = capture->iasio->lpVtbl->createBuffers(capture->iasio, capture->buffer_infos, total_channels, capture->actual_buffer_size, &asio_callbacks);
    if (FAILED(hr)) {
        SAFE_RELEASE(capture->iasio);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to create ASIO buffers");
        return false;
    }
    
    // Interleaved ring buffer size: channels * chunk_size * 8
    size_t ring_size = capture->channels * capture->chunk_size * 8;
    capture->ring_buffer = spsc_audio_ring_buffer_create(ring_size);
    g_capture_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_active_capture = capture;
    
    hr = capture->iasio->lpVtbl->start(capture->iasio);
    if (FAILED(hr)) {
        capture->iasio->lpVtbl->disposeBuffers(capture->iasio);
        SAFE_RELEASE(capture->iasio);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to start ASIO driver");
        return false;
    }
    
    capture->is_running = true;
    return true;
}

static bool asio_capture_read_internal(void* ctx, size_t frames, audio_chunk_t* chunk, backend_error_t* err) {
    (void)err;
    asio_capture_t* capture = (asio_capture_t*)ctx;
    
    size_t requested = frames * capture->channels;
    if (requested > capture->decode_buf_size) {
        capture->decode_buf = (float*)realloc(capture->decode_buf, requested * sizeof(float));
        capture->decode_buf_size = requested;
    }
    
    while (spsc_audio_ring_buffer_get_available_to_read(capture->ring_buffer) < requested) {
        if (WaitForSingleObject(g_capture_event, 100) != WAIT_OBJECT_0) {
            if (!capture->is_running) return false;
        }
    }
    
    size_t consumed = spsc_audio_ring_buffer_consume(capture->ring_buffer, capture->decode_buf, requested);
    if (consumed < requested) {
        memset(capture->decode_buf + consumed, 0, (requested - consumed) * sizeof(float));
    }
    
    for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < capture->channels; c++) {
            audio_chunk_get_channel(chunk, c)[f] = (double)capture->decode_buf[f * capture->channels + c];
        }
    }
    
    chunk->valid_frames = frames;
    return true;
}

static void asio_capture_close_internal(void* ctx) {
    asio_capture_t* capture = (asio_capture_t*)ctx;
    if (capture->iasio) {
        capture->is_running = false;
        capture->iasio->lpVtbl->stop(capture->iasio);
        capture->iasio->lpVtbl->disposeBuffers(capture->iasio);
        SAFE_RELEASE(capture->iasio);
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
    if (capture->buffer_infos) free(capture->buffer_infos);
    if (capture->channel_infos) free(capture->channel_infos);
    if (g_capture_event) {
        CloseHandle(g_capture_event);
        g_capture_event = NULL;
    }
    if (g_active_capture == capture) g_active_capture = NULL;
}

static bool asio_capture_wait_for_data(void* ctx, uint32_t timeout_ms) {
    (void)ctx;
    return WaitForSingleObject(g_capture_event, timeout_ms) == WAIT_OBJECT_0;
}

static void asio_capture_destroy_internal(void* ctx) {
    asio_capture_close_internal(ctx);
    free(ctx);
}

static const capture_backend_vtable_t asio_capture_vtable = {
    asio_capture_open_internal,
    asio_capture_read_internal,
    asio_capture_close_internal,
    NULL,
    NULL,
    NULL,
    asio_capture_wait_for_data,
    asio_capture_destroy_internal
};

capture_backend_t* asio_capture_new(const capture_device_config_t* config, int sample_rate, int chunk_size, backend_error_t* err) {
    (void)err;
    asio_capture_t* capture = (asio_capture_t*)calloc(1, sizeof(asio_capture_t));
    if (!capture) return NULL;
    
    snprintf(capture->device, sizeof(capture->device), "%s", config->device);
    capture->channels = config->channels;
    capture->sample_rate = sample_rate;
    capture->chunk_size = chunk_size;
    capture->format = config->asio_format;
    
    capture_backend_t* backend = (capture_backend_t*)malloc(sizeof(capture_backend_t));
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

static bool asio_playback_open_internal(void* ctx, backend_error_t* err) {
    asio_playback_t* playback = (asio_playback_t*)ctx;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    
    CLSID clsid;
    if (!find_asio_driver_clsid(playback->device, &clsid)) {
        if (err) backend_error_init(err, BACKEND_ERROR_DEVICE_NOT_FOUND, "ASIO playback driver not found");
        return false;
    }
    
    HRESULT hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IASIO, (void**)&playback->iasio);
    if (FAILED(hr)) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to instantiate ASIO driver");
        return false;
    }
    
    if (!playback->iasio->lpVtbl->init(playback->iasio, GetDesktopWindow())) {
        SAFE_RELEASE(playback->iasio);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to initialize ASIO driver");
        return false;
    }
    
    hr = playback->iasio->lpVtbl->setSampleRate(playback->iasio, playback->sample_rate);
    if (FAILED(hr)) {
        SAFE_RELEASE(playback->iasio);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "ASIO sample rate not supported");
        return false;
    }
    
    long min_sz, max_sz, pref_sz, granularity;
    playback->iasio->lpVtbl->getBufferSize(playback->iasio, &min_sz, &max_sz, &pref_sz, &granularity);
    playback->actual_buffer_size = pref_sz;
    
    long num_in = 0, num_out = 0;
    playback->iasio->lpVtbl->getChannels(playback->iasio, &num_in, &num_out);
    
    int total_allocated = num_in + playback->channels;
    playback->buffer_infos = (ASIOBufferInfo*)calloc(total_allocated, sizeof(ASIOBufferInfo));
    playback->channel_infos = (ASIOChannelInfo*)calloc(total_allocated, sizeof(ASIOChannelInfo));
    
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
        playback->iasio->lpVtbl->getChannelInfo(playback->iasio, &playback->channel_infos[idx]);
    }
    
    hr = playback->iasio->lpVtbl->createBuffers(playback->iasio, playback->buffer_infos, total_allocated, playback->actual_buffer_size, &asio_callbacks);
    if (FAILED(hr)) {
        SAFE_RELEASE(playback->iasio);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to create ASIO buffers");
        return false;
    }
    
    size_t ring_size = playback->channels * playback->chunk_size * 8;
    playback->ring_buffer = spsc_audio_ring_buffer_create(ring_size);
    g_active_playback = playback;
    
    hr = playback->iasio->lpVtbl->start(playback->iasio);
    if (FAILED(hr)) {
        playback->iasio->lpVtbl->disposeBuffers(playback->iasio);
        SAFE_RELEASE(playback->iasio);
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to start ASIO driver");
        return false;
    }
    
    playback->is_running = true;
    return true;
}

static bool asio_playback_write_internal(void* ctx, const audio_chunk_t* chunk, backend_error_t* err) {
    (void)err;
    asio_playback_t* playback = (asio_playback_t*)ctx;
    
    size_t requested = chunk->valid_frames * playback->channels;
    if (requested > playback->encode_buf_size) {
        playback->encode_buf = (float*)realloc(playback->encode_buf, requested * sizeof(float));
        playback->encode_buf_size = requested;
    }
    
    for (size_t f = 0; f < chunk->valid_frames; f++) {
        for (int c = 0; c < playback->channels; c++) {
            playback->encode_buf[f * playback->channels + c] = (float)audio_chunk_get_channel(chunk, c)[f];
        }
    }
    
    size_t written = 0;
    while (written < requested) {
        // Since ring buffer holds flat interleaved float array, stride = 1
        size_t available_space = playback->ring_buffer->capacity - spsc_audio_ring_buffer_get_available_to_read(playback->ring_buffer);
        size_t to_write = requested - written;
        if (to_write > available_space) to_write = available_space;
        
        if (to_write > 0) {
            spsc_audio_ring_buffer_write(playback->ring_buffer, playback->encode_buf + written, to_write, 1);
            written += to_write;
        } else {
            Sleep(1);
            if (!playback->is_running) return false;
        }
    }
    return true;
}

static void asio_playback_close_internal(void* ctx) {
    asio_playback_t* playback = (asio_playback_t*)ctx;
    if (playback->iasio) {
        playback->is_running = false;
        playback->iasio->lpVtbl->stop(playback->iasio);
        playback->iasio->lpVtbl->disposeBuffers(playback->iasio);
        SAFE_RELEASE(playback->iasio);
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
    if (playback->buffer_infos) free(playback->buffer_infos);
    if (playback->channel_infos) free(playback->channel_infos);
    if (g_active_playback == playback) g_active_playback = NULL;
}

static size_t asio_playback_get_buffer_level(void* ctx) {
    asio_playback_t* playback = (asio_playback_t*)ctx;
    return playback->ring_buffer ? (spsc_audio_ring_buffer_get_available_to_read(playback->ring_buffer) / playback->channels) : 0;
}

static void asio_playback_destroy_internal(void* ctx) {
    asio_playback_close_internal(ctx);
    free(ctx);
}

static const playback_backend_vtable_t asio_playback_vtable = {
    asio_playback_open_internal,
    asio_playback_write_internal,
    asio_playback_close_internal,
    asio_playback_get_buffer_level,
    NULL,
    NULL,
    NULL,
    NULL,
    asio_playback_destroy_internal
};

playback_backend_t* asio_playback_new(const playback_device_config_t* config, int sample_rate, int chunk_size, backend_error_t* err) {
    (void)err;
    asio_playback_t* playback = (asio_playback_t*)calloc(1, sizeof(asio_playback_t));
    if (!playback) return NULL;
    
    snprintf(playback->device, sizeof(playback->device), "%s", config->device);
    playback->channels = config->channels;
    playback->sample_rate = sample_rate;
    playback->chunk_size = chunk_size;
    playback->format = config->asio_format;
    
    playback_backend_t* backend = (playback_backend_t*)malloc(sizeof(playback_backend_t));
    if (!backend) {
        free(playback);
        return NULL;
    }
    backend->ctx = playback;
    backend->vtable = &asio_playback_vtable;
    return backend;
}

#endif // _WIN32
