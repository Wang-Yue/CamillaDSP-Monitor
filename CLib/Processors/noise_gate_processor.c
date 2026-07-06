/**
 * @file noise_gate_processor.c
 * @brief Implementation of the noise gate processor.
 *
 * Implementation details:
 * - Real-time processing (`noise_gate_processor_process`):
 *   1. Sums monitored channels into scratch buffer using vDSP_vaddD or scalar loop.
 *   2. Envelope Detection: Computes instantaneous dB loudness and smooths it using attack filter when level rises, and release filter when level falls.
 *   3. Gate Threshold Logic: If loudness is below threshold, sets scratch buffer gain to precomputed linear attenuation factor; otherwise sets gain to 1.0 (unity).
 *   4. Multiplies processed channel waveforms by the computed gain curve using vDSP_vmulD or scalar loop.
 */

#include "noise_gate_processor.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif

noise_gate_processor_t* noise_gate_processor_create(const char* name, const noise_gate_parameters_t* params, int sample_rate, size_t chunk_size) {
    if (!params || sample_rate <= 0 || chunk_size == 0) return NULL;

    noise_gate_processor_t* processor = (noise_gate_processor_t*)calloc(1, sizeof(noise_gate_processor_t));
    if (!processor) return NULL;

    if (name) {
        strncpy(processor->name, name, sizeof(processor->name) - 1);
        processor->name[sizeof(processor->name) - 1] = '\0';
    } else {
        strcpy(processor->name, "noisegate");
    }

    processor->scratch_capacity = chunk_size;
    processor->scratch = (double*)calloc(chunk_size, sizeof(double));
    if (!processor->scratch) {
        free(processor);
        return NULL;
    }

    if (params->monitor_channels_count > 0 && params->monitor_channels) {
        processor->monitor_channels_count = params->monitor_channels_count;
        processor->monitor_channels = (int*)calloc(processor->monitor_channels_count, sizeof(int));
        memcpy(processor->monitor_channels, params->monitor_channels, processor->monitor_channels_count * sizeof(int));
    } else {
        processor->monitor_channels_count = (size_t)params->channels;
        processor->monitor_channels = (int*)calloc(processor->monitor_channels_count, sizeof(int));
        for (size_t i = 0; i < processor->monitor_channels_count; i++) {
            processor->monitor_channels[i] = (int)i;
        }
    }

    if (params->process_channels_count > 0 && params->process_channels) {
        processor->process_channels_count = params->process_channels_count;
        processor->process_channels = (int*)calloc(processor->process_channels_count, sizeof(int));
        memcpy(processor->process_channels, params->process_channels, processor->process_channels_count * sizeof(int));
    } else {
        processor->process_channels_count = (size_t)params->channels;
        processor->process_channels = (int*)calloc(processor->process_channels_count, sizeof(int));
        for (size_t i = 0; i < processor->process_channels_count; i++) {
            processor->process_channels[i] = (int)i;
        }
    }

    if (!processor->monitor_channels || !processor->process_channels) {
        noise_gate_processor_free(processor);
        return NULL;
    }

    double srate = (double)sample_rate;
    processor->attack = exp(-1.0 / srate / params->attack);
    processor->release = exp(-1.0 / srate / params->release);
    processor->threshold = params->threshold;
    processor->factor = double_from_db(-params->attenuation);
    processor->prev_loudness = 0.0;

    return processor;
}

void noise_gate_processor_free(noise_gate_processor_t* processor) {
    if (!processor) return;
    free(processor->monitor_channels);
    free(processor->process_channels);
    free(processor->scratch);
    free(processor);
}

void noise_gate_processor_process(noise_gate_processor_t* processor, audio_chunk_t* chunk) {
    if (!processor || !chunk || !processor->scratch) return;
    size_t count = chunk->valid_frames;
    if (count > processor->scratch_capacity) count = processor->scratch_capacity;
    if (count == 0 || processor->monitor_channels_count == 0) return;

    // Step 1: Sum monitored channels into scratch buffer to evaluate overall signal level
    int ch0 = processor->monitor_channels[0];
    const double* src0_base = audio_chunk_get_channel(chunk, ch0);
    if (!src0_base) return;
    memcpy(processor->scratch, src0_base, count * sizeof(double));

    for (size_t ch_idx = 1; ch_idx < processor->monitor_channels_count; ch_idx++) {
        int ch = processor->monitor_channels[ch_idx];
        const double* src_base = audio_chunk_get_channel(chunk, ch);
        if (!src_base) continue;
#ifdef __APPLE__
        vDSP_vaddD(processor->scratch, 1, src_base, 1, processor->scratch, 1, count);
#else
        for (size_t i = 0; i < count; i++) {
            processor->scratch[i] += src_base[i];
        }
#endif
    }

    // Step 2: Envelope Detection (Loudness Estimation with Attack/Release Smoothing)
    double prev = processor->prev_loudness;
    for (size_t i = 0; i < count; i++) {
        double val = 20.0 * log10(fabs(processor->scratch[i]) + 1e-9);
        if (val >= prev) {
            // Signal level rising: apply attack time constant
            val = processor->attack * prev + (1.0 - processor->attack) * val;
        } else {
            // Signal level falling: apply release time constant
            val = processor->release * prev + (1.0 - processor->release) * val;
        }
        prev = val;
        processor->scratch[i] = val;
    }
    processor->prev_loudness = prev;

    // Step 3: Gate Threshold Logic
    for (size_t i = 0; i < count; i++) {
        if (processor->scratch[i] < processor->threshold) {
            // Below threshold: gate closed, apply linear attenuation factor
            processor->scratch[i] = processor->factor;
        } else {
            // Above or equal to threshold: gate open, unity gain (1.0)
            processor->scratch[i] = 1.0;
        }
    }

    // Step 4: Apply gating gain curve to all processed channels
    for (size_t ch_idx = 0; ch_idx < processor->process_channels_count; ch_idx++) {
        int ch = processor->process_channels[ch_idx];
        double* wave = audio_chunk_get_channel(chunk, ch);
        if (!wave) continue;
#ifdef __APPLE__
        vDSP_vmulD(wave, 1, processor->scratch, 1, wave, 1, count);
#else
        for (size_t i = 0; i < count; i++) {
            wave[i] *= processor->scratch[i];
        }
#endif
    }
}


void noise_gate_processor_update_parameters(noise_gate_processor_t* processor, const processor_config_t* config, int sample_rate) {
    if (!processor || !config || sample_rate <= 0) return;
    if (config->type != PROCESSOR_TYPE_NOISE_GATE) return;
    const noise_gate_parameters_t* params = &config->parameters.noise_gate;

    if (params->monitor_channels_count > 0 && params->monitor_channels) {
        free(processor->monitor_channels);
        processor->monitor_channels_count = params->monitor_channels_count;
        processor->monitor_channels = (int*)calloc(processor->monitor_channels_count, sizeof(int));
        memcpy(processor->monitor_channels, params->monitor_channels, processor->monitor_channels_count * sizeof(int));
    }
    if (params->process_channels_count > 0 && params->process_channels) {
        free(processor->process_channels);
        processor->process_channels_count = params->process_channels_count;
        processor->process_channels = (int*)calloc(processor->process_channels_count, sizeof(int));
        memcpy(processor->process_channels, params->process_channels, processor->process_channels_count * sizeof(int));
    }

    double srate = (double)sample_rate;
    processor->attack = exp(-1.0 / srate / params->attack);
    processor->release = exp(-1.0 / srate / params->release);
    processor->threshold = params->threshold;
    processor->factor = double_from_db(-params->attenuation);
}
