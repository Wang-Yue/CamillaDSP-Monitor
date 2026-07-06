// CoreAudio capture backend for macOS
//
// Real-time discipline
// --------------------
// The render callback runs on a high-priority audio thread driven by
// CoreAudio. It is absolutely forbidden to take locks, allocate, or
// otherwise call into the Swift runtime in a way that could block. To
// honour that:
//   - sample rings are SPSC `SPSCAudioRingBuffer<Float>` instances —
//     producer and consumer are wait-free, no `NSLock`.
//   - the AudioBufferList plus its per-channel raw data buffers are
//     preallocated in `open()` and reused for the lifetime of the unit;
//     the render callback only fills the existing struct.

#include "core_audio_capture.h"
#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdatomic.h>

struct core_audio_capture {
    char device_name[256];
    int channels;
    double sample_rate;
    int chunk_size;
    
    AudioUnit audio_unit;
    /// Per-channel SPSC ring buffer of `Float` samples. Render callback
    /// is the producer; `read(frames:)` is the consumer.
    spsc_audio_ring_buffer_t** capture_rings;
    /// Capacity (samples per channel) the rings were sized for.
    /// Whether the audio unit delivers interleaved or non-interleaved
    /// audio. Determined in `open()`; read by the render callback.
    bool is_interleaved;
    
    /// Preallocated AudioBufferList + raw per-buffer storage. Filled in
    /// `open()` after the stream format is known, freed in `close()`.
    /// The render callback re-uses these every invocation — no
    /// allocations on the audio thread.
    AudioBufferList* prealloc_buffer_list;
    void** prealloc_channel_data_pointers;
    int prealloc_bytes_per_channel_buffer;
    int callback_error_count;
    
    /// HAL device the unit is bound to. Captured during `open()` so
    /// `close()` can dispose the rate-change listener without redoing
    /// the lookup (which would race a default-device change).
    AudioDeviceID opened_device_id;
    /// Watches the device's nominal sample rate so the engine can
    /// surface `.captureFormatChange` when something else flips the
    /// device rate at runtime. `nil` until `open()` resolves a device.
    rate_change_watcher_t* rate_watcher;
    /// `true` once `open()` has confirmed the device exposes the
    /// "Internal Adjustable" clock source (BlackHole 0.5.0+) and
    /// successfully selected it. Read by the rate-adjust loop to
    /// decide whether to route corrections to `setPitch(_:)` (the
    /// bit-perfect path) or to fall back to the resampler ratio.
    bool pitch_control_active;
    _Atomic bool is_device_alive;
    
    /// Float scratch used by `read(frames:)` to copy samples out of the
    /// SPSC ring before they're widened to `Double` for the AudioChunk.
    /// Sized to one chunk; reused on every read so the consumer thread
    /// doesn't churn the heap.
    float* read_scratch;
};

static OSStatus capture_alive_listener_callback(AudioObjectID inObjectID, UInt32 inNumberAddresses, const AudioObjectPropertyAddress* inAddresses, void* inClientData) {
    (void)inNumberAddresses;
    (void)inAddresses;
    core_audio_capture_t* capture = (core_audio_capture_t*)inClientData;
    if (!capture) return noErr;
    uint32_t alive = 0;
    uint32_t size = sizeof(uint32_t);
    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioDevicePropertyDeviceIsAlive,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(inObjectID, &addr, 0, NULL, &size, &alive) == noErr) {
        atomic_store_explicit(&capture->is_device_alive, (alive != 0), memory_order_release);
    }
    return noErr;
}

/// CoreAudio render callback for capture. Hot path: must not lock,
/// allocate, or call into Swift runtime in a way that could block.
static OSStatus capture_callback(void* inRefCon,
                                 AudioUnitRenderActionFlags* ioActionFlags,
                                 const AudioTimeStamp* inTimeStamp,
                                 UInt32 inBusNumber,
                                 UInt32 inNumberFrames,
                                 AudioBufferList* ioData) {
    (void)inBusNumber;
    (void)ioData;
    core_audio_capture_t* capture = (core_audio_capture_t*)inRefCon;
    if (!capture || !capture->prealloc_buffer_list || !capture->prealloc_channel_data_pointers || !capture->audio_unit) {
        return noErr;
    }

    AudioBufferList* buffer_list = capture->prealloc_buffer_list;
    uint32_t prealloc_size = (uint32_t)capture->prealloc_bytes_per_channel_buffer;
    for (UInt32 i = 0; i < buffer_list->mNumberBuffers; i++) {
        buffer_list->mBuffers[i].mDataByteSize = prealloc_size;
    }

    OSStatus status = AudioUnitRender(capture->audio_unit, ioActionFlags, inTimeStamp, 1, inNumberFrames, buffer_list);
    if (status != noErr) {
        if (capture->callback_error_count < 3) {
            capture->callback_error_count++;
        }
        return noErr;
    }

    int frame_count = (int)inNumberFrames;
    int actual_frames = frame_count;
    if (capture->is_interleaved) {
        size_t bytes_per_frame = sizeof(float) * capture->channels;
        actual_frames = bytes_per_frame > 0 ? (int)(buffer_list->mBuffers[0].mDataByteSize / bytes_per_frame) : frame_count;
    } else {
        size_t bytes_per_frame = sizeof(float);
        actual_frames = (int)(buffer_list->mBuffers[0].mDataByteSize / bytes_per_frame);
    }

    int frames = actual_frames < frame_count ? actual_frames : frame_count;
    if (frames <= 0) return noErr;

    if (capture->is_interleaved) {
        float* float_ptr = (float*)capture->prealloc_channel_data_pointers[0];
        for (int ch = 0; ch < capture->channels; ch++) {
            spsc_audio_ring_buffer_write(capture->capture_rings[ch], float_ptr + ch, frames, capture->channels);
        }
    } else {
        for (int ch = 0; ch < capture->channels; ch++) {
            float* float_ptr = (float*)capture->prealloc_channel_data_pointers[ch];
            spsc_audio_ring_buffer_write(capture->capture_rings[ch], float_ptr, frames, 1);
        }
    }

    return noErr;
}

// MARK: - Render-callback storage

static void deallocate_render_buffers(core_audio_capture_t* capture) {
    if (capture->prealloc_channel_data_pointers) {
        int num_buffers = capture->prealloc_buffer_list ? (int)capture->prealloc_buffer_list->mNumberBuffers : (capture->is_interleaved ? 1 : capture->channels);
        for (int i = 0; i < num_buffers; i++) {
            free(capture->prealloc_channel_data_pointers[i]);
        }
        free(capture->prealloc_channel_data_pointers);
        capture->prealloc_channel_data_pointers = NULL;
    }
    if (capture->prealloc_buffer_list) {
        free(capture->prealloc_buffer_list);
        capture->prealloc_buffer_list = NULL;
    }
    capture->prealloc_bytes_per_channel_buffer = 0;
}

/// Allocate the AudioBufferList plus per-buffer raw storage that the
/// render callback re-uses on every invocation. Caller must have set
/// `isInterleaved` before this is invoked.
static bool allocate_render_buffers(core_audio_capture_t* capture) {
    deallocate_render_buffers(capture);

    int buffer_frames = capture->chunk_size;
    if (capture->opened_device_id != 0) {
        uint32_t actual_size = 0;
        if (core_audio_device_get_buffer_frame_size(capture->opened_device_id, CORE_AUDIO_SCOPE_INPUT, &actual_size)) {
            if ((int)actual_size > buffer_frames) buffer_frames = (int)actual_size;
        }
    }

    int num_buffers = capture->is_interleaved ? 1 : capture->channels;
    int bytes_per_buffer = capture->is_interleaved ? buffer_frames * capture->channels * sizeof(float) : buffer_frames * sizeof(float);

    size_t list_byte_count = offsetof(AudioBufferList, mBuffers) + num_buffers * sizeof(AudioBuffer);
    capture->prealloc_buffer_list = (AudioBufferList*)calloc(1, list_byte_count);
    capture->prealloc_channel_data_pointers = (void**)calloc(num_buffers, sizeof(void*));
    if (!capture->prealloc_buffer_list || !capture->prealloc_channel_data_pointers) {
        deallocate_render_buffers(capture);
        return false;
    }

    for (int i = 0; i < num_buffers; i++) {
        capture->prealloc_channel_data_pointers[i] = calloc(1, bytes_per_buffer);
        if (!capture->prealloc_channel_data_pointers[i]) {
            deallocate_render_buffers(capture);
            return false;
        }
        capture->prealloc_buffer_list->mBuffers[i].mNumberChannels = capture->is_interleaved ? capture->channels : 1;
        capture->prealloc_buffer_list->mBuffers[i].mDataByteSize = (UInt32)bytes_per_buffer;
        capture->prealloc_buffer_list->mBuffers[i].mData = capture->prealloc_channel_data_pointers[i];
    }
    capture->prealloc_buffer_list->mNumberBuffers = (UInt32)num_buffers;
    capture->prealloc_bytes_per_channel_buffer = bytes_per_buffer;
    return true;
}

static bool vtable_open(void* ctx, backend_error_t* err) { return core_audio_capture_open((core_audio_capture_t*)ctx, err); }
static bool vtable_read(void* ctx, size_t frames, audio_chunk_t* chunk, backend_error_t* err) { return core_audio_capture_read((core_audio_capture_t*)ctx, frames, chunk, err); }
static void vtable_close(void* ctx) { core_audio_capture_close((core_audio_capture_t*)ctx); }
static bool vtable_get_rate(void* ctx, double* out_rate) { return core_audio_capture_get_pending_rate_change((core_audio_capture_t*)ctx, out_rate); }
static bool vtable_pitch_supp(void* ctx) { return core_audio_capture_pitch_control_supported((core_audio_capture_t*)ctx); }
static void vtable_set_pitch(void* ctx, double mult) { core_audio_capture_set_pitch((core_audio_capture_t*)ctx, mult); }
static bool vtable_wait(void* ctx, uint32_t t) { return core_audio_capture_wait((core_audio_capture_t*)ctx, t); }
static void vtable_destroy(void* ctx) { core_audio_capture_destroy((core_audio_capture_t*)ctx); }

static const capture_backend_vtable_t CORE_AUDIO_CAPTURE_VTABLE = {
    .open = vtable_open,
    .read = vtable_read,
    .close = vtable_close,
    .get_pending_rate_change = vtable_get_rate,
    .is_pitch_control_supported = vtable_pitch_supp,
    .set_pitch = vtable_set_pitch,
    .wait_for_data = vtable_wait,
    .destroy = vtable_destroy
};

/// Create a CoreAudio capture backend instance.
capture_backend_t* core_audio_capture_create(const capture_device_config_t* config, int sample_rate, int chunk_size, backend_error_t* err) {
    if (!config) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Config is NULL");
        return NULL;
    }
    core_audio_capture_t* capture = (core_audio_capture_t*)calloc(1, sizeof(core_audio_capture_t));
    if (!capture) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Out of memory");
        return NULL;
    }
    if (config->device[0] != '\0') {
        strncpy(capture->device_name, config->device, sizeof(capture->device_name) - 1);
    }
    capture->channels = config->channels;
    capture->sample_rate = (double)sample_rate;
    capture->chunk_size = chunk_size;
    capture->capture_rings = (spsc_audio_ring_buffer_t**)calloc(config->channels, sizeof(spsc_audio_ring_buffer_t*));
    if (!capture->capture_rings) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Out of memory");
        free(capture);
        return NULL;
    }
    for (int i = 0; i < config->channels; i++) {
        capture->capture_rings[i] = spsc_audio_ring_buffer_create(chunk_size * 4);
        if (!capture->capture_rings[i]) {
            if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Out of memory");
            for (int j = 0; j < i; j++) {
                spsc_audio_ring_buffer_free(capture->capture_rings[j]);
            }
            free(capture->capture_rings);
            free(capture);
            return NULL;
        }
    }
    atomic_init(&capture->is_device_alive, true);
    
    capture_backend_t* backend = (capture_backend_t*)calloc(1, sizeof(capture_backend_t));
    if (!backend) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Out of memory");
        core_audio_capture_destroy(capture);
        return NULL;
    }
    backend->ctx = capture;
    backend->vtable = &CORE_AUDIO_CAPTURE_VTABLE;
    return backend;
}

/// Open the CoreAudio capture device and initialize the AudioUnit and render buffers.
bool core_audio_capture_open(core_audio_capture_t* capture, backend_error_t* err) {
    if (!capture) return false;
    core_audio_capture_close(capture);
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

    OSStatus status = AudioComponentInstanceNew(comp, &capture->audio_unit);
    if (status != noErr || !capture->audio_unit) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to create AudioUnit");
        goto cleanup;
    }

    UInt32 enable_input = 1;
    status = AudioUnitSetProperty(capture->audio_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &enable_input, sizeof(enable_input));
    if (status != noErr) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to enable input");
        goto cleanup;
    }

    UInt32 disable_output = 0;
    status = AudioUnitSetProperty(capture->audio_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &disable_output, sizeof(disable_output));
    if (status != noErr) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to disable output");
        goto cleanup;
    }

    AudioDeviceID dev_id = core_audio_device_id_for_name(capture->device_name[0] ? capture->device_name : NULL, CORE_AUDIO_SCOPE_INPUT);
    capture->opened_device_id = dev_id;
    if (dev_id != 0 && capture->device_name[0]) {
        AudioUnitSetProperty(capture->audio_unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev_id, sizeof(dev_id));
    }
    if (dev_id != 0) {
        core_audio_device_set_nominal_sample_rate(dev_id, capture->sample_rate);
        core_audio_device_set_buffer_frame_size(dev_id, (uint32_t)capture->chunk_size, CORE_AUDIO_SCOPE_INPUT);
        
        AudioObjectPropertyAddress alive_addr = {
            .mSelector = kAudioDevicePropertyDeviceIsAlive,
            .mScope = kAudioObjectPropertyScopeGlobal,
            .mElement = kAudioObjectPropertyElementMain
        };
        AudioObjectAddPropertyListener(dev_id, &alive_addr, capture_alive_listener_callback, capture);
    }

    AudioStreamBasicDescription stream_format = core_audio_device_float32_stream_format(capture->sample_rate, capture->channels, false);
    status = AudioUnitSetProperty(capture->audio_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &stream_format, sizeof(stream_format));
    if (status != noErr) {
        stream_format = core_audio_device_float32_stream_format(capture->sample_rate, capture->channels, true);
        status = AudioUnitSetProperty(capture->audio_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &stream_format, sizeof(stream_format));
        if (status != noErr) {
            if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to set stream format");
            goto cleanup;
        }
    }
    capture->is_interleaved = ((stream_format.mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0);

    UInt32 max_frames = (UInt32)capture->chunk_size;
    if (dev_id != 0) {
        uint32_t actual_size = 0;
        if (core_audio_device_get_buffer_frame_size(dev_id, CORE_AUDIO_SCOPE_INPUT, &actual_size)) {
            if ((int)actual_size > (int)max_frames) max_frames = actual_size;
        }
    }
    AudioUnitSetProperty(capture->audio_unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &max_frames, sizeof(max_frames));

    if (!allocate_render_buffers(capture)) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to allocate render buffers");
        goto cleanup;
    }
    if (!capture->read_scratch) {
        capture->read_scratch = (float*)calloc(capture->chunk_size, sizeof(float));
    }
    if (!capture->read_scratch) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to allocate read scratch buffer");
        goto cleanup;
    }

    AURenderCallbackStruct cb = {
        .inputProc = capture_callback,
        .inputProcRefCon = capture
    };
    status = AudioUnitSetProperty(capture->audio_unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb, sizeof(cb));
    if (status != noErr) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to set callback");
        goto cleanup;
    }

    status = AudioUnitInitialize(capture->audio_unit);
    if (status != noErr) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to initialize AudioUnit");
        goto cleanup;
    }

    status = AudioOutputUnitStart(capture->audio_unit);
    if (status != noErr) {
        if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, "Failed to start AudioUnit");
        goto cleanup;
    }

    if (dev_id != 0 && core_audio_device_has_nominal_sample_rate_property(dev_id)) {
        capture->rate_watcher = rate_change_watcher_create(dev_id, capture->sample_rate);
    }
    if (dev_id != 0 && core_audio_device_select_adjustable_clock_source(dev_id)) {
        capture->pitch_control_active = true;
    }

    open_succeeded = true;
    return true;

cleanup:
    if (!open_succeeded) {
        core_audio_capture_close(capture);
    }
    return false;
}

/// Read a chunk of audio from the capture ring buffers into the provided audio chunk.
bool core_audio_capture_read(core_audio_capture_t* capture, size_t frames, audio_chunk_t* chunk, backend_error_t* err) {
    if (!capture) return false;
    if (!atomic_load_explicit(&capture->is_device_alive, memory_order_acquire)) {
        if (err) backend_error_init(err, BACKEND_ERROR_READ_ERROR, "Capture device disconnected");
        return false;
    }
    for (int ch = 0; ch < capture->channels; ch++) {
        if (spsc_audio_ring_buffer_get_available_to_read(capture->capture_rings[ch]) < frames) {
            return false;
        }
    }
    if (!capture->read_scratch) return false;

    for (int ch = 0; ch < capture->channels; ch++) {
        size_t n = spsc_audio_ring_buffer_consume(capture->capture_rings[ch], capture->read_scratch, frames);
        double* dst_ptr = audio_chunk_get_channel(chunk, ch);
        if (dst_ptr) {
            vDSP_vspdp(capture->read_scratch, 1, dst_ptr, 1, n);
        }
    }
    chunk->valid_frames = frames;
    return true;
}

/// Close the CoreAudio capture device and release HAL resources.
void core_audio_capture_close(core_audio_capture_t* capture) {
    if (!capture) return;
    if (capture->rate_watcher) {
        rate_change_watcher_free(capture->rate_watcher);
        capture->rate_watcher = NULL;
    }
    if (capture->opened_device_id != 0) {
        AudioObjectPropertyAddress alive_addr = {
            .mSelector = kAudioDevicePropertyDeviceIsAlive,
            .mScope = kAudioObjectPropertyScopeGlobal,
            .mElement = kAudioObjectPropertyElementMain
        };
        AudioObjectRemovePropertyListener(capture->opened_device_id, &alive_addr, capture_alive_listener_callback, capture);
    }
    if (capture->audio_unit) {
        AudioOutputUnitStop(capture->audio_unit);
        AudioComponentInstanceDispose(capture->audio_unit);
        capture->audio_unit = NULL;
    }
    deallocate_render_buffers(capture);
    if (capture->read_scratch) {
        free(capture->read_scratch);
        capture->read_scratch = NULL;
    }
    capture->opened_device_id = 0;
}

/// Get any pending sample rate change detected on the capture device.
bool core_audio_capture_get_pending_rate_change(core_audio_capture_t* capture, double* out_rate) {
    if (!capture || !capture->rate_watcher) return false;
    return rate_change_watcher_get_pending_change(capture->rate_watcher, out_rate);
}

/// Check if clock-pitch control is supported on the capture device.
bool core_audio_capture_pitch_control_supported(core_audio_capture_t* capture) {
    return capture ? capture->pitch_control_active : false;
}

/// Apply a clock-pitch correction to the capture device.
void core_audio_capture_set_pitch(core_audio_capture_t* capture, double multiplier) {
    if (!capture || !capture->pitch_control_active || capture->opened_device_id == 0) return;
    core_audio_device_set_pitch(capture->opened_device_id, multiplier);
}

/// Wait for new samples to become available, up to the given timeout.
bool core_audio_capture_wait(core_audio_capture_t* capture, uint32_t timeout_ms) {
    (void)capture;
    usleep(timeout_ms * 1000);
    return false;
}

/// Destroy and free the CoreAudio capture backend.
void core_audio_capture_destroy(core_audio_capture_t* capture) {
    if (!capture) return;
    core_audio_capture_close(capture);
    if (capture->read_scratch) {
        free(capture->read_scratch);
        capture->read_scratch = NULL;
    }
    if (capture->capture_rings) {
        for (int i = 0; i < capture->channels; i++) {
            if (capture->capture_rings[i]) spsc_audio_ring_buffer_free(capture->capture_rings[i]);
        }
        free(capture->capture_rings);
    }
    free(capture);
}
#endif // __APPLE__
