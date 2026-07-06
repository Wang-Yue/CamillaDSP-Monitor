#include "lookahead_limiter.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static double compute_delay_samples(double delay, delay_unit_t unit, int sample_rate) {
    switch (unit) {
    case DELAY_UNIT_MS:
        return delay / 1000.0 * (double)sample_rate;
    case DELAY_UNIT_US:
        return delay / 1000000.0 * (double)sample_rate;
    case DELAY_UNIT_SAMPLES:
        return delay;
    case DELAY_UNIT_MM:
        return delay / 1000.0 * (double)sample_rate / 343.0;
    default:
        return delay;
    }
}

static void configure(const lookahead_limiter_parameters_t* params, int sample_rate, prc_fmt_t* out_limit, int* out_attack_samples, prc_fmt_t* out_release_coeff) {
    double limit_db = params ? params->limit : 0.0;
    *out_limit = prc_fmt_from_db(limit_db);
    delay_unit_t unit = params ? params->unit : DELAY_UNIT_MS;
    double attack = params ? params->attack : 0.0;
    double release = params ? params->release : 0.0;
    *out_attack_samples = (int)round(compute_delay_samples(attack, unit, sample_rate));
    double release_samples = compute_delay_samples(release, unit, sample_rate);
    if (release_samples > 0.0) {
        *out_release_coeff = exp(-1.0 / release_samples);
    } else {
        *out_release_coeff = 0.0;
    }
}

static inline void push_overwrite(lookahead_limiter_filter_t* filter, prc_fmt_t sample) {
    filter->lookahead_data[filter->lookahead_write_index] = sample;
    filter->lookahead_write_index = (filter->lookahead_write_index + 1) % filter->lookahead_capacity;
    filter->lookahead_read_index = (filter->lookahead_read_index + 1) % filter->lookahead_capacity;
}

static inline prc_fmt_t get_occupied(lookahead_limiter_filter_t* filter, size_t idx) {
    size_t real_idx = (filter->lookahead_read_index + idx) % filter->lookahead_capacity;
    return filter->lookahead_data[real_idx];
}

lookahead_limiter_filter_t* lookahead_limiter_filter_create(const char* name, const lookahead_limiter_parameters_t* params, int sample_rate, size_t chunk_size) {
    lookahead_limiter_filter_t* filter = (lookahead_limiter_filter_t*)malloc(sizeof(lookahead_limiter_filter_t));
    if (!filter) return NULL;
    if (name) {
        strncpy(filter->name, name, sizeof(filter->name) - 1);
        filter->name[sizeof(filter->name) - 1] = '\0';
    } else {
        strcpy(filter->name, "lookahead_limiter");
    }

    prc_fmt_t limit;
    int attack_samples;
    prc_fmt_t release_coeff;
    configure(params, sample_rate, &limit, &attack_samples, &release_coeff);

    filter->limit = limit;
    filter->attack_samples = attack_samples;
    filter->release_coeff = release_coeff;
    filter->release_gain = 1.0;

    // Inlined LookaheadBuffer
    size_t lookahead_len = (size_t)sample_rate > chunk_size ? (size_t)sample_rate : chunk_size;
    if (lookahead_len < 1024) lookahead_len = 1024;
    filter->lookahead_capacity = lookahead_len;
    filter->lookahead_data = (prc_fmt_t*)calloc(lookahead_len, sizeof(prc_fmt_t));
    filter->lookahead_read_index = 0;
    filter->lookahead_write_index = 0;

    // Pre-allocated output buffer to avoid heap allocation on the hot path
    size_t out_cap = chunk_size > 8192 ? chunk_size : 8192;
    filter->output_buffer_capacity = out_cap;
    filter->output_buffer = (prc_fmt_t*)calloc(out_cap, sizeof(prc_fmt_t));

    return filter;
}

static void process_slice(lookahead_limiter_filter_t* filter, mutable_waveform_t waveform, size_t len) {
    size_t lookahead_start = filter->lookahead_capacity - filter->attack_samples;
    double peak = 1.0;
    int samples_since_peak = filter->attack_samples + 1;

    // Backward pass
    for (int i = (int)(filter->attack_samples + len) - 1; i >= 0; i--) {
        prc_fmt_t input_sample;
        if (i < filter->attack_samples) {
            input_sample = get_occupied(filter, lookahead_start + i);
        } else {
            input_sample = waveform[i - filter->attack_samples];
        }
        double amplitude = fabs(input_sample);
        double gain = amplitude > filter->limit ? (filter->limit / amplitude) : 1.0;
        double ramp_gain = 1.0;
        if (samples_since_peak <= filter->attack_samples) {
            double ramp = (double)(filter->attack_samples - samples_since_peak) / (double)(filter->attack_samples > 1 ? filter->attack_samples : 1);
            ramp_gain = 1.0 - (ramp * (1.0 - peak));
            samples_since_peak++;
        }
        if (gain < ramp_gain) {
            peak = gain;
            samples_since_peak = 1;
        } else {
            gain = ramp_gain;
        }
        if (i < (int)len) {
            filter->output_buffer[i] = gain;
        }
    }

    // Forward pass
    for (size_t i = 0; i < len; i++) {
        filter->release_gain = pow(filter->release_gain, filter->release_coeff);
        if (filter->output_buffer[i] < filter->release_gain) {
            filter->release_gain = filter->output_buffer[i];
        } else {
            filter->output_buffer[i] = filter->release_gain;
        }
    }

    // Apply gain reduction
    for (size_t i = 0; i < len; i++) {
        prc_fmt_t input_sample;
        if (i < (size_t)filter->attack_samples) {
            input_sample = get_occupied(filter, lookahead_start + i);
        } else {
            input_sample = waveform[i - filter->attack_samples];
        }
        filter->output_buffer[i] *= input_sample;
    }

    // Update lookahead buffer / Output
    for (size_t i = 0; i < len; i++) {
        push_overwrite(filter, waveform[i]);
        waveform[i] = filter->output_buffer[i];
    }
}

void lookahead_limiter_filter_process(lookahead_limiter_filter_t* filter, mutable_waveform_t waveform, size_t count) {
    if (!filter || !waveform || count == 0) return;
    size_t processed = 0;
    while (processed < count) {
        size_t slice = count - processed;
        if (slice > filter->output_buffer_capacity) {
            slice = filter->output_buffer_capacity;
        }
        process_slice(filter, waveform + processed, slice);
        processed += slice;
    }
}

void lookahead_limiter_filter_update_parameters(lookahead_limiter_filter_t* filter, const filter_config_t* config, int sample_rate) {
    if (!filter || !config) return;
    if (config->type != FILTER_TYPE_LOOKAHEAD_LIMITER) return;
    const lookahead_limiter_parameters_t* params = &config->parameters.lookahead_limiter;

    prc_fmt_t limit;
    int attack_samples;
    prc_fmt_t release_coeff;
    configure(params, sample_rate, &limit, &attack_samples, &release_coeff);

    filter->limit = limit;
    filter->attack_samples = attack_samples;
    filter->release_coeff = release_coeff;

    for (int i = 0; i < attack_samples; i++) {
        push_overwrite(filter, 0.0);
    }
}

void lookahead_limiter_filter_free(lookahead_limiter_filter_t* filter) {
    if (!filter) return;
    if (filter->lookahead_data) free(filter->lookahead_data);
    if (filter->output_buffer) free(filter->output_buffer);
    free(filter);
}
