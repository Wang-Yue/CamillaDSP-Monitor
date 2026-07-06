#ifndef CLIB_ENGINE_DSP_ENGINE_H
#define CLIB_ENGINE_DSP_ENGINE_H

#include "dsp_engine_core.h"
#include "Audio/spectrum_analyzer.h"
#include "Audio/audio_history_buffer.h"
#include "Config/configuration.h"
#include "Config/engine_config_types.h"
#include "Config/log_level.h"
#include "Backend/audio_backend.h"
#ifdef __APPLE__
#include "Backend/core_audio_capabilities.h"
#else
#include "Backend/alsa_capabilities.h"
#endif
#include "Server/websocket_server.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    dsp_engine_core_t* core;
    spectrum_analyzer_t* spectrum;
    audio_history_buffer_t* capture_buffer;
    audio_history_buffer_t* playback_buffer;
    double desired_fader_volumes[FADER_COUNT];
    bool desired_fader_mutes[FADER_COUNT];
    processing_stop_reason_t last_stop_reason;
    bool has_last_stop_reason;
} dsp_engine_t;

dsp_engine_t* dsp_engine_create(void);
void dsp_engine_free(dsp_engine_t* engine);

bool dsp_engine_set_config(dsp_engine_t* engine, const char* json, audio_backend_error_t* err);
void dsp_engine_stop(dsp_engine_t* engine);

void dsp_engine_set_fader_volume(dsp_engine_t* engine, fader_t fader, float db);
void dsp_engine_set_fader_mute(dsp_engine_t* engine, fader_t fader, bool mute);
float dsp_engine_get_fader_volume(const dsp_engine_t* engine, fader_t fader);
bool dsp_engine_is_fader_muted(const dsp_engine_t* engine, fader_t fader);
dsp_engine_interface_t* dsp_engine_get_interface(dsp_engine_t* engine);

state_update_t dsp_engine_get_status(const dsp_engine_t* engine);
vu_levels_t dsp_engine_get_vu_levels(const dsp_engine_t* engine);
void dsp_engine_free_vu_levels(vu_levels_t* levels);

spectrum_status_t dsp_engine_get_spectrum(
    dsp_engine_t* engine,
    bool is_capture,
    int channel,
    double min_freq,
    double max_freq,
    size_t n_bins,
    spectrum_result_t* out_result
);

audio_samples_t* dsp_engine_get_samples(dsp_engine_t* engine, bool is_capture, size_t n_frames, audio_backend_error_t* err);
void dsp_engine_free_samples(audio_samples_t* samples);

void dsp_engine_set_log_level(log_level_t level);
int dsp_engine_get_available_devices(const char* backend, bool input, audio_device_t* out_devices, int max_devices);
audio_device_descriptor_t* dsp_engine_get_device_capabilities(const char* backend, const char* device, bool is_capture);
void dsp_engine_free_device_capabilities(audio_device_descriptor_t* desc);
const dsp_config_t* dsp_engine_get_active_config(const dsp_engine_t* engine);
processing_parameters_t* dsp_engine_get_processing_parameters(const dsp_engine_t* engine);

#ifdef __cplusplus
}
#endif

#endif // CLIB_ENGINE_DSP_ENGINE_H
