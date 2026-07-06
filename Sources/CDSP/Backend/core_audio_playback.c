// CoreAudio playback backend for macOS
//
// Real-time discipline
// --------------------
// The render callback runs on a high-priority audio thread driven by
// CoreAudio. It is absolutely forbidden to take locks, allocate, or
// otherwise call into the Swift runtime in a way that could block. To
// honour that:
//   - sample rings are SPSC `SPSCAudioRingBuffer<Float>` instances —
//     producer and consumer are wait-free, no `NSLock`.
//   - the render callback writes directly into the AudioBufferList
//     provided by CoreAudio, consuming from the pre-allocated SPSC rings.

#include "core_audio_playback.h"
#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdatomic.h>

struct core_audio_playback {
    char device_name[256];
    int channels;
    double sample_rate;
    int chunk_size;
    bool exclusive;
    
    AudioUnit audio_unit;
    /// Per-channel SPSC ring buffer of `Float` samples. `write(chunk:)`
    /// is the producer; the render callback is the consumer.
    spsc_audio_ring_buffer_t** playback_rings;
    int ring_buffer_size;
    
    /// HAL device the unit is bound to. Captured from the resolved
    /// device lookup in `open()` so `close()` can release hog mode
    /// without doing the lookup again (which would race a default-
    /// device change).
    AudioDeviceID opened_device_id;
    bool did_acquire_hog_mode;
    /// Watches the device's nominal sample rate so the engine can
    /// surface `.playbackFormatChange` when something else flips the
    /// device rate at runtime.
    rate_change_watcher_t* rate_watcher;
    _Atomic bool is_device_alive;
    _Atomic bool is_paused;
};

static OSStatus playback_alive_listener_callback(AudioObjectID inObjectID, UInt32 inNumberAddresses, const AudioObjectPropertyAddress* inAddresses, void* inClientData) {
    (void)inNumberAddresses;
    (void)inAddresses;
    core_audio_playback_t* playback = (core_audio_playback_t*)inClientData;
    if (!playback) return noErr;
    uint32_t alive = 0;
    uint32_t size = sizeof(uint32_t);
    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioDevicePropertyDeviceIsAlive,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(inObjectID, &addr, 0, NULL, &size, &alive) == noErr) {
        atomic_store_explicit(&playback->is_device_alive, (alive != 0), memory_order_release);
    }
    return noErr;
}

/// CoreAudio render callback for playback. Hot path: must not lock,
/// allocate, or call into Swift runtime in a way that could block.
static OSStatus playback_callback(void* inRefCon,
                                  AudioUnitRenderActionFlags* ioActionFlags,
                                  const AudioTimeStamp* inTimeStamp,
                                  UInt32 inBusNumber,
                                  UInt32 inNumberFrames,
                                  AudioBufferList* ioData) {
    (void)ioActionFlags;
    (void)inTimeStamp;
    (void)inBusNumber;
    core_audio_playback_t* playback = (core_audio_playback_t*)inRefCon;
    if (!playback || !ioData) return noErr;
    
    int frame_count = (int)inNumberFrames;
    
    for (UInt32 ch = 0; ch < ioData->mNumberBuffers; ch++) {
        float* float_ptr = (float*)ioData->mBuffers[ch].mData;
        if (!float_ptr) continue;
        if ((int)ch < playback->channels) {
            size_t copied = spsc_audio_ring_buffer_consume(playback->playback_rings[ch], float_ptr, frame_count);
            if ((int)copied < frame_count) {
                float zero = 0.0f;
                vDSP_vfill(&zero, float_ptr + copied, 1, frame_count - (int)copied);
            }
        } else {
            float zero = 0.0f;
            vDSP_vfill(&zero, float_ptr, 1, frame_count);
        }
    }
    return noErr;
}

static bool vtable_open(void* ctx, backend_error_t* err) { return core_audio_playback_open((core_audio_playback_t*)ctx, err); }
static bool vtable_write(void* ctx, const audio_chunk_t* chunk, backend_error_t* err) { return core_audio_playback_write((core_audio_playback_t*)ctx, chunk, err); }
static void vtable_close(void* ctx) { core_audio_playback_close((core_audio_playback_t*)ctx); }
static size_t vtable_get_level(void* ctx) { return core_audio_playback_get_buffer_level((core_audio_playback_t*)ctx); }
static bool vtable_get_rate(void* ctx, double* out_rate) { return core_audio_playback_get_pending_rate_change((core_audio_playback_t*)ctx, out_rate); }
static bool vtable_prefill(void* ctx, size_t frames, backend_error_t* err) { return core_audio_playback_prefill_silence((core_audio_playback_t*)ctx, frames, err); }
static bool vtable_get_paused(void* ctx) { return core_audio_playback_get_is_paused((core_audio_playback_t*)ctx); }
static void vtable_set_paused(void* ctx, bool paused) { core_audio_playback_set_is_paused((core_audio_playback_t*)ctx, paused); }
static void vtable_destroy(void* ctx) { core_audio_playback_destroy((core_audio_playback_t*)ctx); }

static const playback_backend_vtable_t CORE_AUDIO_PLAYBACK_VTABLE = {
    .open = vtable_open,
    .write = vtable_write,
    .close = vtable_close,
    .get_buffer_level = vtable_get_level,
    .get_pending_rate_change = vtable_get_rate,
    .prefill_silence = vtable_prefill,
    .get_is_paused = vtable_get_paused,
    .set_is_paused = vtable_set_paused,
    .destroy = vtable_destroy
};

/// Create a CoreAudio playback backend instance.
playback_backend_t* core_audio_playback_create(const playback_device_config_t* config, int sample_rate, int chunk_size, backend_error_t* err) {
    if (!config) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Config is NULL");
        return NULL;
    }
    core_audio_playback_t* playback = (core_audio_playback_t*)calloc(1, sizeof(core_audio_playback_t));
    if (!playback) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Out of memory");
        return NULL;
    }
    if (config->device[0] != '\0') {
        strncpy(playback->device_name, config->device, sizeof(playback->device_name) - 1);
    }
    playback->channels = config->channels;
    playback->sample_rate = (double)sample_rate;
    playback->chunk_size = chunk_size;
    playback->exclusive = config->exclusive;
    playback->ring_buffer_size = chunk_size * 8;
    playback->playback_rings = (spsc_audio_ring_buffer_t**)calloc(config->channels, sizeof(spsc_audio_ring_buffer_t*));
    if (!playback->playback_rings) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Out of memory");
        free(playback);
        return NULL;
    }
    for (int i = 0; i < config->channels; i++) {
        playback->playback_rings[i] = spsc_audio_ring_buffer_create(playback->ring_buffer_size);
        if (!playback->playback_rings[i]) {
            if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Out of memory");
            for (int j = 0; j < i; j++) {
                spsc_audio_ring_buffer_free(playback->playback_rings[j]);
            }
            free(playback->playback_rings);
            free(playback);
            return NULL;
        }
    }
    atomic_init(&playback->is_device_alive, true);
    atomic_init(&playback->is_paused, false);
    
    playback_backend_t* backend = (playback_backend_t*)calloc(1, sizeof(playback_backend_t));
    if (!backend) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Out of memory");
        core_audio_playback_destroy(playback);
        return NULL;
    }
    backend->ctx = playback;
    backend->vtable = &CORE_AUDIO_PLAYBACK_VTABLE;
    return backend;
}

/// Open the CoreAudio playback device and initialize output AudioUnit.
bool core_audio_playback_open(core_audio_playback_t* playback, backend_error_t* err) {
    if (!playback) return false;
    core_audio_playback_close(playback);
    bool open_succeeded = false;
    
    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_HALOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
        .componentFlags = 0,
        .componentFlagsMask = 0
    };

    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) {
        if (err) backend_error_init(err, BACKEND_ERROR_DEVICE_NOT_FOUND, "No HAL output component found");
        goto cleanup;
    }

    OSStatus status = AudioComponentInstanceNew(comp, &playback->audio_unit);
    if (status != noErr || !playback->audio_unit) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to create output AudioUnit");
        goto cleanup;
    }

    UInt32 enable_output = 1;
    status = AudioUnitSetProperty(playback->audio_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &enable_output, sizeof(enable_output));
    if (status != noErr) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to enable output");
        goto cleanup;
    }

    UInt32 disable_input = 0;
    status = AudioUnitSetProperty(playback->audio_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &disable_input, sizeof(disable_input));
    if (status != noErr) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to disable input");
        goto cleanup;
    }

    AudioDeviceID dev_id = core_audio_device_id_for_name(playback->device_name[0] ? playback->device_name : NULL, CORE_AUDIO_SCOPE_OUTPUT);
    playback->opened_device_id = dev_id;
    if (dev_id != 0) {
        AudioUnitSetProperty(playback->audio_unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev_id, sizeof(dev_id));
        if (playback->exclusive) {
            pid_t hog_pid = getpid();
            AudioObjectPropertyAddress hog_addr = {
                .mSelector = kAudioDevicePropertyHogMode,
                .mScope = kAudioObjectPropertyScopeGlobal,
                .mElement = kAudioObjectPropertyElementMain
            };
            if (AudioObjectSetPropertyData(dev_id, &hog_addr, 0, NULL, sizeof(pid_t), &hog_pid) == noErr) {
                playback->did_acquire_hog_mode = true;
            }
        }
        core_audio_device_set_nominal_sample_rate(dev_id, playback->sample_rate);
        core_audio_device_set_buffer_frame_size(dev_id, (uint32_t)playback->chunk_size, CORE_AUDIO_SCOPE_OUTPUT);
        
        AudioObjectPropertyAddress alive_addr = {
            .mSelector = kAudioDevicePropertyDeviceIsAlive,
            .mScope = kAudioObjectPropertyScopeGlobal,
            .mElement = kAudioObjectPropertyElementMain
        };
        AudioObjectAddPropertyListener(dev_id, &alive_addr, playback_alive_listener_callback, playback);
    }

    AudioStreamBasicDescription stream_format = core_audio_device_float32_stream_format(playback->sample_rate, playback->channels, false);
    status = AudioUnitSetProperty(playback->audio_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &stream_format, sizeof(stream_format));
    if (status != noErr) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to set playback stream format");
        goto cleanup;
    }

    AURenderCallbackStruct cb = {
        .inputProc = playback_callback,
        .inputProcRefCon = playback
    };
    status = AudioUnitSetProperty(playback->audio_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));
    if (status != noErr) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to set render callback");
        goto cleanup;
    }

    UInt32 max_frames = (UInt32)playback->chunk_size;
    if (dev_id != 0) {
        uint32_t actual_size = 0;
        if (core_audio_device_get_buffer_frame_size(dev_id, CORE_AUDIO_SCOPE_OUTPUT, &actual_size)) {
            if ((int)actual_size > (int)max_frames) max_frames = actual_size;
        }
    }
    AudioUnitSetProperty(playback->audio_unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &max_frames, sizeof(max_frames));

    status = AudioUnitInitialize(playback->audio_unit);
    if (status != noErr) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to initialize output");
        goto cleanup;
    }

    status = AudioOutputUnitStart(playback->audio_unit);
    if (status != noErr) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to start output");
        goto cleanup;
    }

    if (dev_id != 0 && core_audio_device_has_nominal_sample_rate_property(dev_id)) {
        playback->rate_watcher = rate_change_watcher_create(dev_id, playback->sample_rate);
    }

    open_succeeded = true;
    return true;

cleanup:
    if (!open_succeeded) {
        core_audio_playback_close(playback);
    }
    return false;
}

/// Write an audio chunk into the playback ring buffers.
bool core_audio_playback_write(core_audio_playback_t* playback, const audio_chunk_t* chunk, backend_error_t* err) {
    if (!playback) return false;
    if (!atomic_load_explicit(&playback->is_device_alive, memory_order_acquire)) {
        if (err) backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, "Playback device disconnected");
        return false;
    }
    size_t frames = chunk->valid_frames;
    if (frames == 0) return true;

    int usable_channels = playback->channels < (int)chunk->buffers->channels ? playback->channels : (int)chunk->buffers->channels;
    for (int ch = 0; ch < usable_channels; ch++) {
        const double* src_ptr = audio_chunk_get_channel(chunk, ch);
        if (src_ptr) {
            spsc_audio_ring_buffer_append_converting_double_to_float(playback->playback_rings[ch], src_ptr, frames);
        }
    }
    return true;
}

/// Close the CoreAudio playback device and release HAL resources.
void core_audio_playback_close(core_audio_playback_t* playback) {
    if (!playback) return;
    if (playback->rate_watcher) {
        rate_change_watcher_free(playback->rate_watcher);
        playback->rate_watcher = NULL;
    }
    if (playback->opened_device_id != 0) {
        AudioObjectPropertyAddress alive_addr = {
            .mSelector = kAudioDevicePropertyDeviceIsAlive,
            .mScope = kAudioObjectPropertyScopeGlobal,
            .mElement = kAudioObjectPropertyElementMain
        };
        AudioObjectRemovePropertyListener(playback->opened_device_id, &alive_addr, playback_alive_listener_callback, playback);
    }
    if (playback->audio_unit) {
        AudioOutputUnitStop(playback->audio_unit);
        AudioComponentInstanceDispose(playback->audio_unit);
        playback->audio_unit = NULL;
    }
    if (playback->did_acquire_hog_mode && playback->opened_device_id != 0) {
        pid_t pid = -1;
        AudioObjectPropertyAddress addr = {
            .mSelector = kAudioDevicePropertyHogMode,
            .mScope = kAudioObjectPropertyScopeGlobal,
            .mElement = kAudioObjectPropertyElementMain
        };
        AudioObjectSetPropertyData(playback->opened_device_id, &addr, 0, NULL, sizeof(pid_t), &pid);
        playback->did_acquire_hog_mode = false;
    }
    playback->opened_device_id = 0;
}

/// Get the current buffer level in samples.
size_t core_audio_playback_get_buffer_level(core_audio_playback_t* playback) {
    if (!playback || !playback->playback_rings || !playback->playback_rings[0]) return 0;
    return spsc_audio_ring_buffer_get_available_to_read(playback->playback_rings[0]);
}

/// Get any pending sample rate change detected on the playback device.
bool core_audio_playback_get_pending_rate_change(core_audio_playback_t* playback, double* out_rate) {
    if (!playback || !playback->rate_watcher) return false;
    return rate_change_watcher_get_pending_change(playback->rate_watcher, out_rate);
}

/// Push zero samples into the playback ring buffer before real audio arrives.
bool core_audio_playback_prefill_silence(core_audio_playback_t* playback, size_t frames, backend_error_t* err) {
    (void)err;
    if (!playback || frames == 0) return true;
    size_t to_write = frames < (size_t)playback->ring_buffer_size ? frames : (size_t)playback->ring_buffer_size;
    for (int ch = 0; ch < playback->channels; ch++) {
        spsc_audio_ring_buffer_write_silence(playback->playback_rings[ch], to_write);
    }
    return true;
}

/// Check if playback is currently paused.
bool core_audio_playback_get_is_paused(core_audio_playback_t* playback) {
    return playback ? atomic_load_explicit(&playback->is_paused, memory_order_acquire) : false;
}

/// Set playback paused status.
void core_audio_playback_set_is_paused(core_audio_playback_t* playback, bool paused) {
    if (playback) {
        atomic_store_explicit(&playback->is_paused, paused, memory_order_release);
    }
}

/// Destroy and free the CoreAudio playback backend.
void core_audio_playback_destroy(core_audio_playback_t* playback) {
    if (!playback) return;
    core_audio_playback_close(playback);
    if (playback->playback_rings) {
        for (int i = 0; i < playback->channels; i++) {
            if (playback->playback_rings[i]) spsc_audio_ring_buffer_free(playback->playback_rings[i]);
        }
        free(playback->playback_rings);
    }
    free(playback);
}
#endif // __APPLE__
