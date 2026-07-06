// Comparison tests for C filter implementations against camilladsp's
// reference. Drives the `cdsp_filter_compare` Rust harness.

#include "test_support.h"
#include "../../Sources/CDSP/Filters/filter.h"
#include "../../Sources/CDSP/Filters/biquad.h"
#include "../../Sources/CDSP/Filters/gain.h"
#include "../../Sources/CDSP/Filters/volume.h"
#include "../../Sources/CDSP/Filters/loudness.h"
#include "../../Sources/CDSP/Filters/convolution.h"
#include "../../Sources/CDSP/Filters/delay.h"
#include "../../Sources/CDSP/Filters/biquad_combo.h"
#include "../../Sources/CDSP/Filters/diffeq.h"
#include "../../Sources/CDSP/Filters/dither.h"
#include "../../Sources/CDSP/Filters/limiter.h"
#include "../../Sources/CDSP/Filters/lookahead_limiter.h"
#include "../../Sources/CDSP/Mixer/mixer.h"
#include "../../Sources/CDSP/Processors/processor.h"
#include "../../Sources/CDSP/Processors/compressor_processor.h"
#include "../../Sources/CDSP/Processors/noise_gate_processor.h"
#include "../../Sources/CDSP/Processors/race_processor.h"
#include "../../Sources/CDSP/Audio/audio_chunk.h"
#include "../../Sources/CDSP/Audio/double_helpers.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CHUNK_SIZE 1024
#define SAMPLE_RATE 48000
#define NBR_FRAMES (16 * CHUNK_SIZE)

// MARK: - Helpers

static int write_raw(const double* data, size_t count, const char* path) {
    FILE* fp = fopen(path, "wb");
    if (!fp) return -1;
    size_t written = fwrite(data, sizeof(double), count, fp);
    fclose(fp);
    return (written == count) ? 0 : -1;
}

static double* read_raw(const char* path, size_t* out_count) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0 || size % sizeof(double) != 0) {
        fclose(fp);
        return NULL;
    }
    size_t count = size / sizeof(double);
    double* data = (double*)malloc(count * sizeof(double));
    if (!data) {
        fclose(fp);
        return NULL;
    }
    size_t read_cnt = fread(data, sizeof(double), count, fp);
    fclose(fp);
    if (read_cnt != count) {
        free(data);
        return NULL;
    }
    if (out_count) *out_count = count;
    return data;
}

static const char* get_harness_binary(void) {
    const char* env = getenv("CDSP_FILTER_BIN");
    if (env && access(env, X_OK) == 0) {
        return env;
    }
    const char* paths[] = {
        "../Tests/RustHarnesses/target/release/cdsp_filter_compare",
        "Tests/RustHarnesses/target/release/cdsp_filter_compare",
        "/Users/wangyue/CamillaDSP-Monitor/Tests/RustHarnesses/target/release/cdsp_filter_compare"
    };
    for (size_t i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {
        if (access(paths[i], X_OK) == 0) {
            return paths[i];
        }
    }
    return NULL;
}

static bool run_harness_arr(const char* argv[], size_t argc) {
    const char* bin = get_harness_binary();
    if (!bin) {
        printf("⚠️ skipping: harness not found — build with `cd ~/cdsp_filter_compare && cargo build --release`\n");
        return false;
    }
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "\"%s\"", bin);
    for (size_t i = 0; i < argc; i++) {
        strncat(cmd, " \"", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, "\"", sizeof(cmd) - strlen(cmd) - 1);
    }
    strncat(cmd, " 2>&1", sizeof(cmd) - strlen(cmd) - 1);

    FILE* fp = popen(cmd, "r");
    if (!fp) {
        printf("⚠️ skipping: popen failed for harness\n");
        return false;
    }
    char output[4096] = {0};
    size_t len = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (len < sizeof(output) - 1) {
            strncat(output, line, sizeof(output) - len - 1);
            len = strlen(output);
        }
    }
    int status = pclose(fp);
    if (status != 0) {
        printf("harness exited with status %d: %s\n", status, output);
        printf("  [FAIL] %s:%d: harness execution failed\n", __FILE__, __LINE__);
        g_test_failures++;
        return false;
    }
    return true;
}

#define RUN_HARNESS(...) run_harness_arr((const char*[]){__VA_ARGS__}, sizeof((const char*[]){__VA_ARGS__})/sizeof(const char*))

typedef struct {
    uint64_t state;
} seeded_rng_t;

static uint64_t seeded_rng_next(seeded_rng_t* rng) {
    rng->state = rng->state * 6364136223846793005ULL + 1442695040888963407ULL;
    return rng->state;
}

static double seeded_rng_random_double(seeded_rng_t* rng, double min_val, double max_val) {
    uint64_t val = seeded_rng_next(rng);
    double u = (val >> 11) * (1.0 / 9007199254740992.0);
    return min_val + u * (max_val - min_val);
}

static void make_test_signal(double* out, size_t count) {
    seeded_rng_t rng = { 0xCDD5AA42DEADBEEFULL };
    double f1 = 200.0;
    double f2 = 1500.0;
    double f3 = 8000.0;
    for (size_t i = 0; i < count; i++) {
        double t = (double)i / (double)SAMPLE_RATE;
        out[i] = 0.4 * sin(2.0 * M_PI * f1 * t)
               + 0.3 * sin(2.0 * M_PI * f2 * t)
               + 0.2 * sin(2.0 * M_PI * f3 * t)
               + 0.05 * seeded_rng_random_double(&rng, -1.0, 1.0);
    }
}

// MARK: - Biquad

static void compare_biquad(double b0, double b1, double b2, double a1, double a2, const char* label) {
    double* input = (double*)malloc(NBR_FRAMES * sizeof(double));
    make_test_signal(input, NBR_FRAMES);
    char in_path[256], ref_path[256];
    snprintf(in_path, sizeof(in_path), "/tmp/cdsp_biquad_%s_in.raw", label);
    snprintf(ref_path, sizeof(ref_path), "/tmp/cdsp_biquad_%s_ref.raw", label);
    write_raw(input, NBR_FRAMES, in_path);

    char a1_s[64], a2_s[64], b0_s[64], b1_s[64], b2_s[64], sr_s[64], cs_s[64];
    snprintf(a1_s, sizeof(a1_s), "%.17g", a1);
    snprintf(a2_s, sizeof(a2_s), "%.17g", a2);
    snprintf(b0_s, sizeof(b0_s), "%.17g", b0);
    snprintf(b1_s, sizeof(b1_s), "%.17g", b1);
    snprintf(b2_s, sizeof(b2_s), "%.17g", b2);
    snprintf(sr_s, sizeof(sr_s), "%d", SAMPLE_RATE);
    snprintf(cs_s, sizeof(cs_s), "%d", CHUNK_SIZE);

    if (!RUN_HARNESS("biquad", a1_s, a2_s, b0_s, b1_s, b2_s, sr_s, cs_s, in_path, ref_path)) {
        free(input);
        return;
    }
    size_t ref_count = 0;
    double* ref = read_raw(ref_path, &ref_count);
    ASSERT_TRUE(ref != NULL);
    ASSERT_EQ(NBR_FRAMES, ref_count);

    biquad_coefficients_t coeffs = { .b0 = b0, .b1 = b1, .b2 = b2, .a1 = a1, .a2 = a2 };
    biquad_filter_t* filter = biquad_filter_create("test_bq", &coeffs);
    ASSERT_TRUE(filter != NULL);

    double* swift_out = (double*)malloc(NBR_FRAMES * sizeof(double));
    memcpy(swift_out, input, NBR_FRAMES * sizeof(double));

    for (size_t idx = 0; idx < NBR_FRAMES; idx += CHUNK_SIZE) {
        size_t end = (idx + CHUNK_SIZE < NBR_FRAMES) ? (idx + CHUNK_SIZE) : NBR_FRAMES;
        biquad_filter_process(filter, &swift_out[idx], end - idx);
    }

    double max_abs_diff = 0.0;
    double sum_sq = 0.0;
    for (size_t i = 0; i < NBR_FRAMES; i++) {
        double d = swift_out[i] - ref[i];
        if (fabs(d) > max_abs_diff) max_abs_diff = fabs(d);
        sum_sq += d * d;
    }
    double rms = sqrt(sum_sq / (double)NBR_FRAMES);
    printf("[biquad %s] maxAbsDiff=%.3e rms=%.3e (n=%zu)\n", label, max_abs_diff, rms, (size_t)NBR_FRAMES);
    ASSERT_TRUE(max_abs_diff < 1e-13);

    biquad_filter_free(filter);
    free(input);
    free(ref);
    free(swift_out);
}

TEST(Biquad_RawCoeffs_Lowpass1kHz) {
    double b0 = 0.004244741301241303;
    double b1 = 0.008489482602482605;
    double b2 = 0.004244741301241303;
    double a1 = -1.864844640491105;
    double a2 = 0.8818236057002321;
    compare_biquad(b0, b1, b2, a1, a2, "lowpass-1k");
}

TEST(Biquad_RawCoeffs_Highpass5kHz) {
    double b0 = 0.7392382866526886;
    double b1 = -1.4784765733053772;
    double b2 = 0.7392382866526886;
    double a1 = -1.4042598022895725;
    double a2 = 0.5526933443211819;
    compare_biquad(b0, b1, b2, a1, a2, "highpass-5k");
}

TEST(Biquad_RawCoeffs_Peaking2kHz) {
    double b0 = 1.0480378925069767;
    double b1 = -1.9266017680029408;
    double b2 = 0.9043155506796712;
    double a1 = -1.9266017680029408;
    double a2 = 0.9523534431866478;
    compare_biquad(b0, b1, b2, a1, a2, "peaking-2k");
}

// MARK: - Gain

static void compare_gain(double gain_db, bool inverted, bool mute, const char* label) {
    double* input = (double*)malloc(NBR_FRAMES * sizeof(double));
    make_test_signal(input, NBR_FRAMES);
    char in_path[256], ref_path[256];
    snprintf(in_path, sizeof(in_path), "/tmp/cdsp_gain_%s_in.raw", label);
    snprintf(ref_path, sizeof(ref_path), "/tmp/cdsp_gain_%s_ref.raw", label);
    write_raw(input, NBR_FRAMES, in_path);

    char gain_s[64], cs_s[64];
    snprintf(gain_s, sizeof(gain_s), "%.17g", gain_db);
    snprintf(cs_s, sizeof(cs_s), "%d", CHUNK_SIZE);

    if (!RUN_HARNESS("gain", gain_s, inverted ? "1" : "0", mute ? "1" : "0", cs_s, in_path, ref_path)) {
        free(input);
        return;
    }
    size_t ref_count = 0;
    double* ref = read_raw(ref_path, &ref_count);
    ASSERT_TRUE(ref != NULL);
    ASSERT_EQ(NBR_FRAMES, ref_count);

    gain_parameters_t params = {
        .gain = gain_db,
        .has_gain = true,
        .scale = GAIN_SCALE_DB,
        .inverted = inverted,
        .mute = mute
    };
    gain_filter_t* filter = gain_filter_create("test_gain", &params);
    ASSERT_TRUE(filter != NULL);

    double* swift_out = (double*)malloc(NBR_FRAMES * sizeof(double));
    memcpy(swift_out, input, NBR_FRAMES * sizeof(double));

    for (size_t idx = 0; idx < NBR_FRAMES; idx += CHUNK_SIZE) {
        size_t end = (idx + CHUNK_SIZE < NBR_FRAMES) ? (idx + CHUNK_SIZE) : NBR_FRAMES;
        gain_filter_process(filter, &swift_out[idx], end - idx);
    }

    double max_abs_diff = 0.0;
    for (size_t i = 0; i < NBR_FRAMES; i++) {
        double d = fabs(swift_out[i] - ref[i]);
        if (d > max_abs_diff) max_abs_diff = d;
    }
    printf("[gain %s] maxAbsDiff=%.3e (n=%zu)\n", label, max_abs_diff, (size_t)NBR_FRAMES);
    ASSERT_TRUE(max_abs_diff < 1e-12);

    gain_filter_free(filter);
    free(input);
    free(ref);
    free(swift_out);
}

TEST(Gain_Plus6dB) {
    compare_gain(6.0, false, false, "+6dB");
}

TEST(Gain_Minus12dB_Inverted) {
    compare_gain(-12.0, true, false, "-12dB-inv");
}

TEST(Gain_Mute) {
    compare_gain(3.0, false, true, "mute");
}

// MARK: - Volume

static void compare_volume(double current_volume_db, bool mute, const char* label) {
    double* input = (double*)malloc(NBR_FRAMES * sizeof(double));
    make_test_signal(input, NBR_FRAMES);
    char in_path[256], ref_path[256];
    snprintf(in_path, sizeof(in_path), "/tmp/cdsp_volume_%s_in.raw", label);
    snprintf(ref_path, sizeof(ref_path), "/tmp/cdsp_volume_%s_ref.raw", label);
    write_raw(input, NBR_FRAMES, in_path);

    char vol_s[64], sr_s[64], cs_s[64];
    snprintf(vol_s, sizeof(vol_s), "%.17g", current_volume_db);
    snprintf(sr_s, sizeof(sr_s), "%d", SAMPLE_RATE);
    snprintf(cs_s, sizeof(cs_s), "%d", CHUNK_SIZE);

    if (!RUN_HARNESS("volume", vol_s, mute ? "1" : "0", sr_s, cs_s, in_path, ref_path)) {
        free(input);
        return;
    }
    size_t ref_count = 0;
    double* ref = read_raw(ref_path, &ref_count);
    ASSERT_TRUE(ref != NULL);
    ASSERT_EQ(NBR_FRAMES, ref_count);

    processing_parameters_t* params = processing_parameters_create(1, 1);
    processing_parameters_set_target_volume(params, current_volume_db);
    processing_parameters_set_muted(params, mute);
    processing_parameters_set_current_volume(params, mute ? -100.0 : current_volume_db);

    volume_parameters_t vol_params = {
        .ramp_time = 0.0,
        .has_ramp_time = true,
        .limit = 50.0,
        .has_limit = true,
        .fader = FADER_MAIN
    };
    volume_filter_t* filter = volume_filter_create("test_vol", &vol_params, SAMPLE_RATE, CHUNK_SIZE, params);
    ASSERT_TRUE(filter != NULL);

    double* swift_out = (double*)malloc(NBR_FRAMES * sizeof(double));
    memcpy(swift_out, input, NBR_FRAMES * sizeof(double));

    for (size_t idx = 0; idx < NBR_FRAMES; idx += CHUNK_SIZE) {
        size_t end = (idx + CHUNK_SIZE < NBR_FRAMES) ? (idx + CHUNK_SIZE) : NBR_FRAMES;
        volume_filter_prepare_chunk(filter);
        volume_filter_process(filter, &swift_out[idx], end - idx);
        volume_filter_advance_ramp(filter);
    }

    double max_abs_diff = 0.0;
    for (size_t i = 0; i < NBR_FRAMES; i++) {
        double d = fabs(swift_out[i] - ref[i]);
        if (d > max_abs_diff) max_abs_diff = d;
    }
    printf("[volume %s] maxAbsDiff=%.3e (n=%zu)\n", label, max_abs_diff, (size_t)NBR_FRAMES);
    ASSERT_TRUE(max_abs_diff < 1e-12);

    volume_filter_free(filter);
    processing_parameters_free(params);
    free(input);
    free(ref);
    free(swift_out);
}

TEST(Volume_Plus3dB) {
    compare_volume(3.0, false, "+3dB");
}

TEST(Volume_Minus20dB) {
    compare_volume(-20.0, false, "-20dB");
}

TEST(Volume_Mute) {
    compare_volume(0.0, true, "mute");
}

// MARK: - Loudness

static void compare_loudness(double volume_db, double reference_db, double high_boost, double low_boost, bool attenuate_mid, const char* label) {
    double* input = (double*)malloc(NBR_FRAMES * sizeof(double));
    make_test_signal(input, NBR_FRAMES);
    char in_path[256], ref_path[256];
    snprintf(in_path, sizeof(in_path), "/tmp/cdsp_loudness_%s_in.raw", label);
    snprintf(ref_path, sizeof(ref_path), "/tmp/cdsp_loudness_%s_ref.raw", label);
    write_raw(input, NBR_FRAMES, in_path);

    char vol_s[64], ref_s[64], hb_s[64], lb_s[64], sr_s[64], cs_s[64];
    snprintf(vol_s, sizeof(vol_s), "%.17g", volume_db);
    snprintf(ref_s, sizeof(ref_s), "%.17g", reference_db);
    snprintf(hb_s, sizeof(hb_s), "%.17g", high_boost);
    snprintf(lb_s, sizeof(lb_s), "%.17g", low_boost);
    snprintf(sr_s, sizeof(sr_s), "%d", SAMPLE_RATE);
    snprintf(cs_s, sizeof(cs_s), "%d", CHUNK_SIZE);

    if (!RUN_HARNESS("loudness", vol_s, ref_s, hb_s, lb_s, attenuate_mid ? "1" : "0", sr_s, cs_s, in_path, ref_path)) {
        free(input);
        return;
    }
    size_t ref_count = 0;
    double* ref = read_raw(ref_path, &ref_count);
    ASSERT_TRUE(ref != NULL);
    ASSERT_EQ(NBR_FRAMES, ref_count);

    loudness_parameters_t lp = {
        .reference_level = reference_db,
        .has_reference_level = true,
        .high_boost = high_boost,
        .has_high_boost = true,
        .low_boost = low_boost,
        .has_low_boost = true,
        .attenuate_mid = attenuate_mid,
        .fader = FADER_MAIN
    };
    processing_parameters_t* params = processing_parameters_create(1, 1);
    processing_parameters_set_current_volume(params, volume_db);

    loudness_filter_t* filter = loudness_filter_create("test_loud", &lp, SAMPLE_RATE, params);
    ASSERT_TRUE(filter != NULL);
    double primer[8] = {0};
    loudness_filter_process(filter, primer, 8);

    double* swift_out = (double*)malloc(NBR_FRAMES * sizeof(double));
    memcpy(swift_out, input, NBR_FRAMES * sizeof(double));

    for (size_t idx = 0; idx < NBR_FRAMES; idx += CHUNK_SIZE) {
        size_t end = (idx + CHUNK_SIZE < NBR_FRAMES) ? (idx + CHUNK_SIZE) : NBR_FRAMES;
        loudness_filter_process(filter, &swift_out[idx], end - idx);
    }

    double max_abs_diff = 0.0;
    double sum_sq = 0.0;
    for (size_t i = 0; i < NBR_FRAMES; i++) {
        double d = swift_out[i] - ref[i];
        if (fabs(d) > max_abs_diff) max_abs_diff = fabs(d);
        sum_sq += d * d;
    }
    double rms = sqrt(sum_sq / (double)NBR_FRAMES);
    printf("[loudness %s] maxAbsDiff=%.3e rms=%.3e (n=%zu)\n", label, max_abs_diff, rms, (size_t)NBR_FRAMES);
    ASSERT_TRUE(max_abs_diff < 1e-9);

    loudness_filter_free(filter);
    processing_parameters_free(params);
    free(input);
    free(ref);
    free(swift_out);
}

TEST(Loudness_BelowReference_Active) {
    compare_loudness(-35.0, -25.0, 10.0, 10.0, false, "below-ref");
}

TEST(Loudness_AtReference_NoBoost) {
    compare_loudness(-25.0, -25.0, 10.0, 10.0, false, "at-ref");
}

TEST(Loudness_AttenuateMid) {
    compare_loudness(-45.0, -25.0, 10.0, 10.0, true, "attenuate-mid");
}

// MARK: - Mixer

TEST(Mixer_Vs_AnalyticalReference_StereoToMono) {
    mixer_source_t src0 = { .channel = 0, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB };
    mixer_source_t src1 = { .channel = 1, .gain = -6.0, .has_gain = true, .scale = GAIN_SCALE_DB };
    mixer_source_t sources[2] = { src0, src1 };
    mixer_mapping_t map = { .dest = 0, .sources_count = 2, .sources = sources };
    mixer_config_t config = { .channels_in = 2, .channels_out = 1, .mapping_count = 1, .mapping = &map };
    audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);
    ASSERT_TRUE(mixer != NULL);

    seeded_rng_t rng = { 0xC0FFEE };
    double* l = (double*)malloc(1024 * sizeof(double));
    double* r = (double*)malloc(1024 * sizeof(double));
    for (int i = 0; i < 1024; i++) {
        l[i] = seeded_rng_random_double(&rng, -1.0, 1.0);
        r[i] = seeded_rng_random_double(&rng, -1.0, 1.0);
    }
    audio_chunk_t* chunk = audio_chunk_create(1024, 2);
    double* ch0 = audio_chunk_get_channel(chunk, 0);
    double* ch1 = audio_chunk_get_channel(chunk, 1);
    memcpy(ch0, l, 1024 * sizeof(double));
    memcpy(ch1, r, 1024 * sizeof(double));
    chunk->valid_frames = 1024;

    audio_chunk_t* out = audio_mixer_process_chunk(mixer, chunk);
    ASSERT_TRUE(out != NULL);
    double* out_ch0 = audio_chunk_get_channel(out, 0);

    double gain_r = pow(10.0, -6.0 / 20.0);
    for (int i = 0; i < 1024; i++) {
        double expected = l[i] + gain_r * r[i];
        ASSERT_NEAR(expected, out_ch0[i], 1e-12);
    }

    audio_chunk_free(chunk);
    audio_chunk_free(out);
    audio_mixer_free(mixer);
    free(l);
    free(r);
}

TEST(Mixer_Vs_AnalyticalReference_LinearScale) {
    mixer_source_t src0 = { .channel = 0, .gain = 0.5, .has_gain = true, .scale = GAIN_SCALE_LINEAR };
    mixer_source_t src1 = { .channel = 1, .gain = 0.25, .has_gain = true, .scale = GAIN_SCALE_LINEAR, .inverted = true };
    mixer_source_t sources[2] = { src0, src1 };
    mixer_mapping_t map = { .dest = 0, .sources_count = 2, .sources = sources };
    mixer_config_t config = { .channels_in = 2, .channels_out = 1, .mapping_count = 1, .mapping = &map };
    audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);
    ASSERT_TRUE(mixer != NULL);

    double* l = (double*)malloc(128 * sizeof(double));
    double* r = (double*)malloc(128 * sizeof(double));
    for (int i = 0; i < 128; i++) {
        l[i] = (double)i * 0.01;
        r[i] = (double)i * -0.005;
    }
    audio_chunk_t* chunk = audio_chunk_create(128, 2);
    double* ch0 = audio_chunk_get_channel(chunk, 0);
    double* ch1 = audio_chunk_get_channel(chunk, 1);
    memcpy(ch0, l, 128 * sizeof(double));
    memcpy(ch1, r, 128 * sizeof(double));
    chunk->valid_frames = 128;

    audio_chunk_t* out = audio_mixer_process_chunk(mixer, chunk);
    ASSERT_TRUE(out != NULL);
    double* out_ch0 = audio_chunk_get_channel(out, 0);

    for (int i = 0; i < 128; i++) {
        double expected = 0.5 * l[i] - 0.25 * r[i];
        ASSERT_NEAR(expected, out_ch0[i], 1e-12);
    }

    audio_chunk_free(chunk);
    audio_chunk_free(out);
    audio_mixer_free(mixer);
    free(l);
    free(r);
}

TEST(Mixer_MutedSource_ProducesSilenceFromThatSource) {
    mixer_source_t src0 = { .channel = 0, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB };
    mixer_source_t src1 = { .channel = 1, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB, .mute = true };
    mixer_source_t sources[2] = { src0, src1 };
    mixer_mapping_t map = { .dest = 0, .sources_count = 2, .sources = sources };
    mixer_config_t config = { .channels_in = 2, .channels_out = 1, .mapping_count = 1, .mapping = &map };
    audio_mixer_t* mixer = audio_mixer_create("mixer", &config, 2048);
    ASSERT_TRUE(mixer != NULL);

    double* l = (double*)malloc(64 * sizeof(double));
    double* r = (double*)malloc(64 * sizeof(double));
    for (int i = 0; i < 64; i++) {
        l[i] = (double)i;
        r[i] = 999.0;
    }
    audio_chunk_t* chunk = audio_chunk_create(64, 2);
    double* ch0 = audio_chunk_get_channel(chunk, 0);
    double* ch1 = audio_chunk_get_channel(chunk, 1);
    memcpy(ch0, l, 64 * sizeof(double));
    memcpy(ch1, r, 64 * sizeof(double));
    chunk->valid_frames = 64;

    audio_chunk_t* out = audio_mixer_process_chunk(mixer, chunk);
    ASSERT_TRUE(out != NULL);
    double* out_ch0 = audio_chunk_get_channel(out, 0);

    for (int i = 0; i < 64; i++) {
        ASSERT_NEAR(l[i], out_ch0[i], 1e-12);
    }

    audio_chunk_free(chunk);
    audio_chunk_free(out);
    audio_mixer_free(mixer);
    free(l);
    free(r);
}

// MARK: - Convolution

TEST(Convolution_Vs_Rust_RandomIR) {
    const char* label = "conv-random";
    double* input = (double*)malloc(NBR_FRAMES * sizeof(double));
    make_test_signal(input, NBR_FRAMES);
    char in_path[256], ref_path[256], coeffs_path[256];
    snprintf(in_path, sizeof(in_path), "/tmp/cdsp_conv_%s_in.raw", label);
    snprintf(ref_path, sizeof(ref_path), "/tmp/cdsp_conv_%s_ref.raw", label);
    snprintf(coeffs_path, sizeof(coeffs_path), "/tmp/cdsp_conv_%s_coeffs.raw", label);
    write_raw(input, NBR_FRAMES, in_path);

    seeded_rng_t rng = { 0x123456789ABCDEF0ULL };
    double* coeffs = (double*)malloc(2000 * sizeof(double));
    for (int i = 0; i < 2000; i++) {
        coeffs[i] = seeded_rng_random_double(&rng, -1.0, 1.0);
    }
    write_raw(coeffs, 2000, coeffs_path);

    char cs_s[64];
    snprintf(cs_s, sizeof(cs_s), "%d", CHUNK_SIZE);
    if (!RUN_HARNESS("conv", cs_s, coeffs_path, in_path, ref_path)) {
        free(input);
        free(coeffs);
        return;
    }
    size_t ref_count = 0;
    double* ref = read_raw(ref_path, &ref_count);
    ASSERT_TRUE(ref != NULL);
    ASSERT_EQ(NBR_FRAMES, ref_count);

    conv_parameters_t params = {0};
    params.type = CONV_TYPE_VALUES;
    params.values = coeffs;
    params.values_count = 2000;

    convolution_filter_t* filter = convolution_filter_create("test_conv", &params, CHUNK_SIZE);
    ASSERT_TRUE(filter != NULL);

    double* swift_out = (double*)malloc(NBR_FRAMES * sizeof(double));
    memcpy(swift_out, input, NBR_FRAMES * sizeof(double));

    for (size_t idx = 0; idx < NBR_FRAMES; idx += CHUNK_SIZE) {
        size_t end = (idx + CHUNK_SIZE < NBR_FRAMES) ? (idx + CHUNK_SIZE) : NBR_FRAMES;
        convolution_filter_process(filter, &swift_out[idx], end - idx);
    }

    double max_abs_diff = 0.0;
    double sum_sq = 0.0;
    for (size_t i = 0; i < NBR_FRAMES; i++) {
        double d = swift_out[i] - ref[i];
        if (fabs(d) > max_abs_diff) max_abs_diff = fabs(d);
        sum_sq += d * d;
    }
    double rms = sqrt(sum_sq / (double)NBR_FRAMES);
    printf("[conv %s] maxAbsDiff=%.3e rms=%.3e (n=%zu)\n", label, max_abs_diff, rms, (size_t)NBR_FRAMES);
    ASSERT_TRUE(max_abs_diff < 1e-13);

    convolution_filter_free(filter);
    free(input);
    free(coeffs);
    free(ref);
    free(swift_out);
}

// MARK: - Delay

static const char* delay_unit_to_str(delay_unit_t unit) {
    switch (unit) {
        case DELAY_UNIT_MS: return "ms";
        case DELAY_UNIT_US: return "us";
        case DELAY_UNIT_SAMPLES: return "samples";
        case DELAY_UNIT_MM: return "mm";
        default: return "ms";
    }
}

static void compare_delay(double delay, delay_unit_t unit, bool subsample, const char* label) {
    double* input = (double*)malloc(NBR_FRAMES * sizeof(double));
    make_test_signal(input, NBR_FRAMES);
    char in_path[256], ref_path[256];
    snprintf(in_path, sizeof(in_path), "/tmp/cdsp_delay_%s_in.raw", label);
    snprintf(ref_path, sizeof(ref_path), "/tmp/cdsp_delay_%s_ref.raw", label);
    write_raw(input, NBR_FRAMES, in_path);

    char del_s[64], sr_s[64], cs_s[64];
    snprintf(del_s, sizeof(del_s), "%.17g", delay);
    snprintf(sr_s, sizeof(sr_s), "%d", SAMPLE_RATE);
    snprintf(cs_s, sizeof(cs_s), "%d", CHUNK_SIZE);

    if (!RUN_HARNESS("delay", del_s, delay_unit_to_str(unit), subsample ? "1" : "0", sr_s, cs_s, in_path, ref_path)) {
        free(input);
        return;
    }
    size_t ref_count = 0;
    double* ref = read_raw(ref_path, &ref_count);
    ASSERT_TRUE(ref != NULL);
    ASSERT_EQ(NBR_FRAMES, ref_count);

    delay_parameters_t params = {
        .delay = delay,
        .unit = unit,
        .subsample = subsample
    };
    delay_filter_t* filter = delay_filter_create("test_delay", &params, SAMPLE_RATE);
    ASSERT_TRUE(filter != NULL);

    double* swift_out = (double*)malloc(NBR_FRAMES * sizeof(double));
    memcpy(swift_out, input, NBR_FRAMES * sizeof(double));

    for (size_t idx = 0; idx < NBR_FRAMES; idx += CHUNK_SIZE) {
        size_t end = (idx + CHUNK_SIZE < NBR_FRAMES) ? (idx + CHUNK_SIZE) : NBR_FRAMES;
        delay_filter_process(filter, &swift_out[idx], end - idx);
    }

    double max_abs_diff = 0.0;
    for (size_t i = 0; i < NBR_FRAMES; i++) {
        double d = fabs(swift_out[i] - ref[i]);
        if (d > max_abs_diff) max_abs_diff = d;
    }
    printf("[delay %s] maxAbsDiff=%.3e\n", label, max_abs_diff);
    ASSERT_TRUE(max_abs_diff < 1e-12);

    delay_filter_free(filter);
    free(input);
    free(ref);
    free(swift_out);
}

TEST(Delay_IntegerSamples) {
    compare_delay(50.0, DELAY_UNIT_SAMPLES, false, "50samples");
}

TEST(Delay_Subsample_FirstOrder) {
    compare_delay(0.6, DELAY_UNIT_SAMPLES, true, "0.6samples");
}

TEST(Delay_Subsample_SecondOrder) {
    compare_delay(2.3, DELAY_UNIT_SAMPLES, true, "2.3samples");
}

TEST(Delay_Milliseconds) {
    compare_delay(1.5, DELAY_UNIT_MS, true, "1.5ms");
}

// MARK: - Biquad Combo

static void compare_biquad_combo_filter(biquad_combo_type_t type, double freq, int order, const char* type_str, const char* label) {
    double* input = (double*)malloc(NBR_FRAMES * sizeof(double));
    make_test_signal(input, NBR_FRAMES);
    char in_path[256], ref_path[256];
    snprintf(in_path, sizeof(in_path), "/tmp/cdsp_combo_%s_in.raw", label);
    snprintf(ref_path, sizeof(ref_path), "/tmp/cdsp_combo_%s_ref.raw", label);
    write_raw(input, NBR_FRAMES, in_path);

    char freq_s[64], order_s[64], sr_s[64], cs_s[64];
    snprintf(freq_s, sizeof(freq_s), "%.17g", freq);
    snprintf(order_s, sizeof(order_s), "%d", order);
    snprintf(sr_s, sizeof(sr_s), "%d", SAMPLE_RATE);
    snprintf(cs_s, sizeof(cs_s), "%d", CHUNK_SIZE);

    if (!RUN_HARNESS("biquad_combo", type_str, freq_s, order_s, sr_s, cs_s, in_path, ref_path)) {
        free(input);
        return;
    }
    size_t ref_count = 0;
    double* ref = read_raw(ref_path, &ref_count);
    ASSERT_TRUE(ref != NULL);
    ASSERT_EQ(NBR_FRAMES, ref_count);

    biquad_combo_parameters_t params = {0};
    params.type = type;
    params.freq = freq;
    params.has_freq = true;
    params.order = order;
    params.has_order = true;

    biquad_combo_filter_t* filter = biquad_combo_filter_create("test_combo", &params, SAMPLE_RATE);
    ASSERT_TRUE(filter != NULL);

    double* swift_out = (double*)malloc(NBR_FRAMES * sizeof(double));
    memcpy(swift_out, input, NBR_FRAMES * sizeof(double));

    for (size_t idx = 0; idx < NBR_FRAMES; idx += CHUNK_SIZE) {
        size_t end = (idx + CHUNK_SIZE < NBR_FRAMES) ? (idx + CHUNK_SIZE) : NBR_FRAMES;
        biquad_combo_filter_process(filter, &swift_out[idx], end - idx);
    }

    double max_abs_diff = 0.0;
    for (size_t i = 0; i < NBR_FRAMES; i++) {
        double d = fabs(swift_out[i] - ref[i]);
        if (d > max_abs_diff) max_abs_diff = d;
    }
    printf("[biquad_combo %s] maxAbsDiff=%.3e\n", label, max_abs_diff);
    ASSERT_TRUE(max_abs_diff < 1e-12);

    biquad_combo_filter_free(filter);
    free(input);
    free(ref);
    free(swift_out);
}

static void compare_biquad_combo_tilt(double gain, const char* label) {
    double* input = (double*)malloc(NBR_FRAMES * sizeof(double));
    make_test_signal(input, NBR_FRAMES);
    char in_path[256], ref_path[256];
    snprintf(in_path, sizeof(in_path), "/tmp/cdsp_combo_%s_in.raw", label);
    snprintf(ref_path, sizeof(ref_path), "/tmp/cdsp_combo_%s_ref.raw", label);
    write_raw(input, NBR_FRAMES, in_path);

    char gain_s[64], sr_s[64], cs_s[64];
    snprintf(gain_s, sizeof(gain_s), "%.17g", gain);
    snprintf(sr_s, sizeof(sr_s), "%d", SAMPLE_RATE);
    snprintf(cs_s, sizeof(cs_s), "%d", CHUNK_SIZE);

    if (!RUN_HARNESS("biquad_combo", "tilt", gain_s, sr_s, cs_s, in_path, ref_path)) {
        free(input);
        return;
    }
    size_t ref_count = 0;
    double* ref = read_raw(ref_path, &ref_count);
    ASSERT_TRUE(ref != NULL);
    ASSERT_EQ(NBR_FRAMES, ref_count);

    biquad_combo_parameters_t params = {0};
    params.type = BIQUAD_COMBO_TYPE_TILT;
    params.gain = gain;
    params.has_gain = true;

    biquad_combo_filter_t* filter = biquad_combo_filter_create("test_tilt", &params, SAMPLE_RATE);
    ASSERT_TRUE(filter != NULL);

    double* swift_out = (double*)malloc(NBR_FRAMES * sizeof(double));
    memcpy(swift_out, input, NBR_FRAMES * sizeof(double));

    for (size_t idx = 0; idx < NBR_FRAMES; idx += CHUNK_SIZE) {
        size_t end = (idx + CHUNK_SIZE < NBR_FRAMES) ? (idx + CHUNK_SIZE) : NBR_FRAMES;
        biquad_combo_filter_process(filter, &swift_out[idx], end - idx);
    }

    double max_abs_diff = 0.0;
    for (size_t i = 0; i < NBR_FRAMES; i++) {
        double d = fabs(swift_out[i] - ref[i]);
        if (d > max_abs_diff) max_abs_diff = d;
    }
    printf("[biquad_combo %s] maxAbsDiff=%.3e\n", label, max_abs_diff);
    ASSERT_TRUE(max_abs_diff < 1e-12);

    biquad_combo_filter_free(filter);
    free(input);
    free(ref);
    free(swift_out);
}

static void compare_biquad_combo_geq(double freq_min, double freq_max, const double* gains, size_t gains_count, double epsilon, const char* label) {
    double* input = (double*)malloc(NBR_FRAMES * sizeof(double));
    make_test_signal(input, NBR_FRAMES);
    char in_path[256], ref_path[256];
    snprintf(in_path, sizeof(in_path), "/tmp/cdsp_combo_%s_in.raw", label);
    snprintf(ref_path, sizeof(ref_path), "/tmp/cdsp_combo_%s_ref.raw", label);
    write_raw(input, NBR_FRAMES, in_path);

    char fmin_s[64], fmax_s[64], sr_s[64], cs_s[64];
    snprintf(fmin_s, sizeof(fmin_s), "%.17g", freq_min);
    snprintf(fmax_s, sizeof(fmax_s), "%.17g", freq_max);
    snprintf(sr_s, sizeof(sr_s), "%d", SAMPLE_RATE);
    snprintf(cs_s, sizeof(cs_s), "%d", CHUNK_SIZE);

    char gains_str[512] = {0};
    for (size_t i = 0; i < gains_count; i++) {
        char g_s[64];
        snprintf(g_s, sizeof(g_s), "%.17g", gains[i]);
        if (i > 0) strncat(gains_str, ",", sizeof(gains_str) - strlen(gains_str) - 1);
        strncat(gains_str, g_s, sizeof(gains_str) - strlen(gains_str) - 1);
    }

    if (!RUN_HARNESS("biquad_combo", "graphic_equalizer", fmin_s, fmax_s, gains_str, sr_s, cs_s, in_path, ref_path)) {
        free(input);
        return;
    }
    size_t ref_count = 0;
    double* ref = read_raw(ref_path, &ref_count);
    ASSERT_TRUE(ref != NULL);
    ASSERT_EQ(NBR_FRAMES, ref_count);

    biquad_combo_parameters_t params = {0};
    params.type = BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER;
    params.freq_min = freq_min;
    params.has_freq_min = true;
    params.freq_max = freq_max;
    params.has_freq_max = true;
    params.gains = (double*)gains;
    params.gains_count = gains_count;

    biquad_combo_filter_t* filter = biquad_combo_filter_create("test_geq", &params, SAMPLE_RATE);
    ASSERT_TRUE(filter != NULL);

    double* swift_out = (double*)malloc(NBR_FRAMES * sizeof(double));
    memcpy(swift_out, input, NBR_FRAMES * sizeof(double));

    for (size_t idx = 0; idx < NBR_FRAMES; idx += CHUNK_SIZE) {
        size_t end = (idx + CHUNK_SIZE < NBR_FRAMES) ? (idx + CHUNK_SIZE) : NBR_FRAMES;
        biquad_combo_filter_process(filter, &swift_out[idx], end - idx);
    }

    double max_abs_diff = 0.0;
    for (size_t i = 0; i < NBR_FRAMES; i++) {
        double d = fabs(swift_out[i] - ref[i]);
        if (d > max_abs_diff) max_abs_diff = d;
    }
    printf("[biquad_combo %s] maxAbsDiff=%.3e\n", label, max_abs_diff);
    ASSERT_TRUE(max_abs_diff < epsilon);

    biquad_combo_filter_free(filter);
    free(input);
    free(ref);
    free(swift_out);
}

static void compare_biquad_combo_peq5(double fls, double qls, double gls,
                                      double fp1, double qp1, double gp1,
                                      double fp2, double qp2, double gp2,
                                      double fp3, double qp3, double gp3,
                                      double fhs, double qhs, double ghs,
                                      const char* label) {
    double* input = (double*)malloc(NBR_FRAMES * sizeof(double));
    make_test_signal(input, NBR_FRAMES);
    char in_path[256], ref_path[256];
    snprintf(in_path, sizeof(in_path), "/tmp/cdsp_combo_%s_in.raw", label);
    snprintf(ref_path, sizeof(ref_path), "/tmp/cdsp_combo_%s_ref.raw", label);
    write_raw(input, NBR_FRAMES, in_path);

    char fls_s[64], qls_s[64], gls_s[64];
    char fp1_s[64], qp1_s[64], gp1_s[64];
    char fp2_s[64], qp2_s[64], gp2_s[64];
    char fp3_s[64], qp3_s[64], gp3_s[64];
    char fhs_s[64], qhs_s[64], ghs_s[64];
    char sr_s[64], cs_s[64];

    snprintf(fls_s, sizeof(fls_s), "%.17g", fls); snprintf(qls_s, sizeof(qls_s), "%.17g", qls); snprintf(gls_s, sizeof(gls_s), "%.17g", gls);
    snprintf(fp1_s, sizeof(fp1_s), "%.17g", fp1); snprintf(qp1_s, sizeof(qp1_s), "%.17g", qp1); snprintf(gp1_s, sizeof(gp1_s), "%.17g", gp1);
    snprintf(fp2_s, sizeof(fp2_s), "%.17g", fp2); snprintf(qp2_s, sizeof(qp2_s), "%.17g", qp2); snprintf(gp2_s, sizeof(gp2_s), "%.17g", gp2);
    snprintf(fp3_s, sizeof(fp3_s), "%.17g", fp3); snprintf(qp3_s, sizeof(qp3_s), "%.17g", qp3); snprintf(gp3_s, sizeof(gp3_s), "%.17g", gp3);
    snprintf(fhs_s, sizeof(fhs_s), "%.17g", fhs); snprintf(qhs_s, sizeof(qhs_s), "%.17g", qhs); snprintf(ghs_s, sizeof(ghs_s), "%.17g", ghs);
    snprintf(sr_s, sizeof(sr_s), "%d", SAMPLE_RATE);
    snprintf(cs_s, sizeof(cs_s), "%d", CHUNK_SIZE);

    if (!RUN_HARNESS("biquad_combo", "five_point_peq",
                     fls_s, qls_s, gls_s,
                     fp1_s, qp1_s, gp1_s,
                     fp2_s, qp2_s, gp2_s,
                     fp3_s, qp3_s, gp3_s,
                     fhs_s, qhs_s, ghs_s,
                     sr_s, cs_s, in_path, ref_path)) {
        free(input);
        return;
    }
    size_t ref_count = 0;
    double* ref = read_raw(ref_path, &ref_count);
    ASSERT_TRUE(ref != NULL);
    ASSERT_EQ(NBR_FRAMES, ref_count);

    biquad_combo_parameters_t params = {0};
    params.type = BIQUAD_COMBO_TYPE_FIVE_POINT_PEQ;
    params.fls = fls; params.qls = qls; params.gls = gls;
    params.has_fls = true; params.has_qls = true; params.has_gls = true;
    params.fp1 = fp1; params.qp1 = qp1; params.gp1 = gp1;
    params.has_fp1 = true; params.has_qp1 = true; params.has_gp1 = true;
    params.fp2 = fp2; params.qp2 = qp2; params.gp2 = gp2;
    params.has_fp2 = true; params.has_qp2 = true; params.has_gp2 = true;
    params.fp3 = fp3; params.qp3 = qp3; params.gp3 = gp3;
    params.has_fp3 = true; params.has_qp3 = true; params.has_gp3 = true;
    params.fhs = fhs; params.qhs = qhs; params.ghs = ghs;
    params.has_fhs = true; params.has_qhs = true; params.has_ghs = true;

    biquad_combo_filter_t* filter = biquad_combo_filter_create("test_peq5", &params, SAMPLE_RATE);
    ASSERT_TRUE(filter != NULL);

    double* swift_out = (double*)malloc(NBR_FRAMES * sizeof(double));
    memcpy(swift_out, input, NBR_FRAMES * sizeof(double));

    for (size_t idx = 0; idx < NBR_FRAMES; idx += CHUNK_SIZE) {
        size_t end = (idx + CHUNK_SIZE < NBR_FRAMES) ? (idx + CHUNK_SIZE) : NBR_FRAMES;
        biquad_combo_filter_process(filter, &swift_out[idx], end - idx);
    }

    double max_abs_diff = 0.0;
    for (size_t i = 0; i < NBR_FRAMES; i++) {
        double d = fabs(swift_out[i] - ref[i]);
        if (d > max_abs_diff) max_abs_diff = d;
    }
    printf("[biquad_combo %s] maxAbsDiff=%.3e\n", label, max_abs_diff);
    ASSERT_TRUE(max_abs_diff < 1e-12);

    biquad_combo_filter_free(filter);
    free(input);
    free(ref);
    free(swift_out);
}

TEST(BiquadCombo_ButterworthLowpass) {
    compare_biquad_combo_filter(BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS, 1200.0, 4, "butterworth_lowpass", "bw-lp");
}

TEST(BiquadCombo_ButterworthHighpass) {
    compare_biquad_combo_filter(BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS, 600.0, 3, "butterworth_highpass", "bw-hp");
}

TEST(BiquadCombo_LinkwitzRileyLowpass) {
    compare_biquad_combo_filter(BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS, 2000.0, 4, "linkwitz_riley_lowpass", "lr-lp");
}

TEST(BiquadCombo_LinkwitzRileyHighpass) {
    compare_biquad_combo_filter(BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS, 1500.0, 2, "linkwitz_riley_highpass", "lr-hp");
}

TEST(BiquadCombo_Tilt) {
    compare_biquad_combo_tilt(4.5, "tilt");
}

TEST(BiquadCombo_GraphicEqualizer) {
    double gains[] = {1.0, -2.0, 3.0, -1.5, 0.5};
    compare_biquad_combo_geq(20.0, 20000.0, gains, 5, 1e-7, "geq");
}

TEST(BiquadCombo_FivePointPeq) {
    compare_biquad_combo_peq5(80.0, 0.707, 3.0,
                              250.0, 1.5, -2.0,
                              1000.0, 2.0, 1.5,
                              4000.0, 1.0, -1.0,
                              12000.0, 0.707, 2.5,
                              "peq5");
}

// MARK: - DiffEq

TEST(DiffEq_SimpleIIR) {
    double a[] = {1.0, -1.864844640491105, 0.8818236057002321};
    double b[] = {0.004244741301241303, 0.008489482602482605, 0.004244741301241303};
    double* input = (double*)malloc(NBR_FRAMES * sizeof(double));
    make_test_signal(input, NBR_FRAMES);
    char in_path[256], ref_path[256];
    snprintf(in_path, sizeof(in_path), "/tmp/cdsp_diffeq_in.raw");
    snprintf(ref_path, sizeof(ref_path), "/tmp/cdsp_diffeq_ref.raw");
    write_raw(input, NBR_FRAMES, in_path);

    char a_str[256], b_str[256], cs_s[64];
    snprintf(a_str, sizeof(a_str), "%.17g,%.17g,%.17g", a[0], a[1], a[2]);
    snprintf(b_str, sizeof(b_str), "%.17g,%.17g,%.17g", b[0], b[1], b[2]);
    snprintf(cs_s, sizeof(cs_s), "%d", CHUNK_SIZE);

    if (!RUN_HARNESS("diff_eq", a_str, b_str, cs_s, in_path, ref_path)) {
        free(input);
        return;
    }
    size_t ref_count = 0;
    double* ref = read_raw(ref_path, &ref_count);
    ASSERT_TRUE(ref != NULL);
    ASSERT_EQ(NBR_FRAMES, ref_count);

    diff_eq_parameters_t params = {
        .a = a,
        .a_count = 3,
        .b = b,
        .b_count = 3
    };
    diffeq_filter_t* filter = diffeq_filter_create("test_diffeq", &params);
    ASSERT_TRUE(filter != NULL);

    double* swift_out = (double*)malloc(NBR_FRAMES * sizeof(double));
    memcpy(swift_out, input, NBR_FRAMES * sizeof(double));

    for (size_t idx = 0; idx < NBR_FRAMES; idx += CHUNK_SIZE) {
        size_t end = (idx + CHUNK_SIZE < NBR_FRAMES) ? (idx + CHUNK_SIZE) : NBR_FRAMES;
        diffeq_filter_process(filter, &swift_out[idx], end - idx);
    }

    double max_abs_diff = 0.0;
    for (size_t i = 0; i < NBR_FRAMES; i++) {
        double d = fabs(swift_out[i] - ref[i]);
        if (d > max_abs_diff) max_abs_diff = d;
    }
    printf("[diffeq] maxAbsDiff=%.3e\n", max_abs_diff);
    ASSERT_TRUE(max_abs_diff < 1e-12);

    diffeq_filter_free(filter);
    free(input);
    free(ref);
    free(swift_out);
}

// MARK: - Dither

TEST(Dither_None) {
    double* input = (double*)malloc(NBR_FRAMES * sizeof(double));
    make_test_signal(input, NBR_FRAMES);
    char in_path[256], ref_path[256];
    snprintf(in_path, sizeof(in_path), "/tmp/cdsp_dither_none_in.raw");
    snprintf(ref_path, sizeof(ref_path), "/tmp/cdsp_dither_none_ref.raw");
    write_raw(input, NBR_FRAMES, in_path);

    char cs_s[64];
    snprintf(cs_s, sizeof(cs_s), "%d", CHUNK_SIZE);
    if (!RUN_HARNESS("dither", "none", "16", cs_s, in_path, ref_path)) {
        free(input);
        return;
    }
    size_t ref_count = 0;
    double* ref = read_raw(ref_path, &ref_count);
    ASSERT_TRUE(ref != NULL);
    ASSERT_EQ(NBR_FRAMES, ref_count);

    dither_parameters_t params = {0};
    params.type = DITHER_TYPE_NONE;
    params.bits = 16;
    dither_filter_t* filter = dither_filter_create("test_dither", &params);
    ASSERT_TRUE(filter != NULL);

    double* swift_out = (double*)malloc(NBR_FRAMES * sizeof(double));
    memcpy(swift_out, input, NBR_FRAMES * sizeof(double));

    for (size_t idx = 0; idx < NBR_FRAMES; idx += CHUNK_SIZE) {
        size_t end = (idx + CHUNK_SIZE < NBR_FRAMES) ? (idx + CHUNK_SIZE) : NBR_FRAMES;
        dither_filter_process(filter, &swift_out[idx], end - idx);
    }

    double max_abs_diff = 0.0;
    for (size_t i = 0; i < NBR_FRAMES; i++) {
        double d = fabs(swift_out[i] - ref[i]);
        if (d > max_abs_diff) max_abs_diff = d;
    }
    printf("[dither none] maxAbsDiff=%.3e\n", max_abs_diff);
    ASSERT_TRUE(max_abs_diff < 1e-12);

    dither_filter_free(filter);
    free(input);
    free(ref);
    free(swift_out);
}

// MARK: - Limiter

static void compare_limiter(double clip_limit, bool soft_clip, const char* label) {
    double* input = (double*)malloc(NBR_FRAMES * sizeof(double));
    make_test_signal(input, NBR_FRAMES);
    char in_path[256], ref_path[256];
    snprintf(in_path, sizeof(in_path), "/tmp/cdsp_limiter_%s_in.raw", label);
    snprintf(ref_path, sizeof(ref_path), "/tmp/cdsp_limiter_%s_ref.raw", label);
    write_raw(input, NBR_FRAMES, in_path);

    char lim_s[64], cs_s[64];
    snprintf(lim_s, sizeof(lim_s), "%.17g", clip_limit);
    snprintf(cs_s, sizeof(cs_s), "%d", CHUNK_SIZE);

    if (!RUN_HARNESS("limiter", lim_s, soft_clip ? "1" : "0", cs_s, in_path, ref_path)) {
        free(input);
        return;
    }
    size_t ref_count = 0;
    double* ref = read_raw(ref_path, &ref_count);
    ASSERT_TRUE(ref != NULL);
    ASSERT_EQ(NBR_FRAMES, ref_count);

    limiter_parameters_t params = {
        .clip_limit = clip_limit,
        .soft_clip = soft_clip
    };
    limiter_filter_t* filter = limiter_filter_create("test_limiter", &params);
    ASSERT_TRUE(filter != NULL);

    double* swift_out = (double*)malloc(NBR_FRAMES * sizeof(double));
    memcpy(swift_out, input, NBR_FRAMES * sizeof(double));

    for (size_t idx = 0; idx < NBR_FRAMES; idx += CHUNK_SIZE) {
        size_t end = (idx + CHUNK_SIZE < NBR_FRAMES) ? (idx + CHUNK_SIZE) : NBR_FRAMES;
        limiter_filter_process(filter, &swift_out[idx], end - idx);
    }

    double max_abs_diff = 0.0;
    for (size_t i = 0; i < NBR_FRAMES; i++) {
        double d = fabs(swift_out[i] - ref[i]);
        if (d > max_abs_diff) max_abs_diff = d;
    }
    printf("[limiter %s] maxAbsDiff=%.3e\n", label, max_abs_diff);
    ASSERT_TRUE(max_abs_diff < 1e-12);

    limiter_filter_free(filter);
    free(input);
    free(ref);
    free(swift_out);
}

TEST(Limiter_HardClip) {
    compare_limiter(-3.0, false, "hard");
}

TEST(Limiter_SoftClip) {
    compare_limiter(-1.5, true, "soft");
}

// MARK: - Lookahead Limiter

static void compare_lookahead_limiter(double limit, double attack, double release, delay_unit_t unit, const char* label) {
    double* input = (double*)malloc(NBR_FRAMES * sizeof(double));
    make_test_signal(input, NBR_FRAMES);
    char in_path[256], ref_path[256];
    snprintf(in_path, sizeof(in_path), "/tmp/cdsp_lookahead_%s_in.raw", label);
    snprintf(ref_path, sizeof(ref_path), "/tmp/cdsp_lookahead_%s_ref.raw", label);
    write_raw(input, NBR_FRAMES, in_path);

    char lim_s[64], att_s[64], rel_s[64], sr_s[64], cs_s[64];
    snprintf(lim_s, sizeof(lim_s), "%.17g", limit);
    snprintf(att_s, sizeof(att_s), "%.17g", attack);
    snprintf(rel_s, sizeof(rel_s), "%.17g", release);
    snprintf(sr_s, sizeof(sr_s), "%d", SAMPLE_RATE);
    snprintf(cs_s, sizeof(cs_s), "%d", CHUNK_SIZE);

    if (!RUN_HARNESS("lookahead_limiter", lim_s, att_s, rel_s, delay_unit_to_str(unit), sr_s, cs_s, in_path, ref_path)) {
        free(input);
        return;
    }
    size_t ref_count = 0;
    double* ref = read_raw(ref_path, &ref_count);
    ASSERT_TRUE(ref != NULL);
    ASSERT_EQ(NBR_FRAMES, ref_count);

    lookahead_limiter_parameters_t params = {
        .limit = limit,
        .attack = attack,
        .release = release,
        .unit = unit
    };
    lookahead_limiter_filter_t* filter = lookahead_limiter_filter_create("test_lookahead", &params, SAMPLE_RATE, CHUNK_SIZE);
    ASSERT_TRUE(filter != NULL);

    double* swift_out = (double*)malloc(NBR_FRAMES * sizeof(double));
    memcpy(swift_out, input, NBR_FRAMES * sizeof(double));

    for (size_t idx = 0; idx < NBR_FRAMES; idx += CHUNK_SIZE) {
        size_t end = (idx + CHUNK_SIZE < NBR_FRAMES) ? (idx + CHUNK_SIZE) : NBR_FRAMES;
        lookahead_limiter_filter_process(filter, &swift_out[idx], end - idx);
    }

    double max_abs_diff = 0.0;
    for (size_t i = 0; i < NBR_FRAMES; i++) {
        double d = fabs(swift_out[i] - ref[i]);
        if (d > max_abs_diff) max_abs_diff = d;
    }
    printf("[lookahead %s] maxAbsDiff=%.3e\n", label, max_abs_diff);
    ASSERT_TRUE(max_abs_diff < 1e-5);

    lookahead_limiter_filter_free(filter);
    free(input);
    free(ref);
    free(swift_out);
}

TEST(LookaheadLimiter_Basic) {
    compare_lookahead_limiter(-1.0, 4.0, 20.0, DELAY_UNIT_SAMPLES, "basic");
}

TEST(LookaheadLimiter_Instant) {
    compare_lookahead_limiter(-2.0, 0.0, 0.0, DELAY_UNIT_SAMPLES, "instant");
}

// MARK: - Processors

TEST(Compressor_Vs_RustReference) {
    const char* label = "compressor-compare";
    double* input = (double*)malloc(NBR_FRAMES * sizeof(double));
    make_test_signal(input, NBR_FRAMES);
    char in_path[256], ref_path[256];
    snprintf(in_path, sizeof(in_path), "/tmp/cdsp_comp_%s_in.raw", label);
    snprintf(ref_path, sizeof(ref_path), "/tmp/cdsp_comp_%s_ref.raw", label);
    write_raw(input, NBR_FRAMES, in_path);

    double attack = 0.005, release = 0.05, threshold = -10.0, factor = 3.0, makeup_gain = 2.0, clip_limit = -1.0;
    char att_s[64], rel_s[64], thr_s[64], fac_s[64], mk_s[64], lim_s[64], sr_s[64], cs_s[64];
    snprintf(att_s, sizeof(att_s), "%.17g", attack);
    snprintf(rel_s, sizeof(rel_s), "%.17g", release);
    snprintf(thr_s, sizeof(thr_s), "%.17g", threshold);
    snprintf(fac_s, sizeof(fac_s), "%.17g", factor);
    snprintf(mk_s, sizeof(mk_s), "%.17g", makeup_gain);
    snprintf(lim_s, sizeof(lim_s), "%.17g", clip_limit);
    snprintf(sr_s, sizeof(sr_s), "%d", SAMPLE_RATE);
    snprintf(cs_s, sizeof(cs_s), "%d", CHUNK_SIZE);

    if (!RUN_HARNESS("compressor", att_s, rel_s, thr_s, fac_s, mk_s, "1", lim_s, sr_s, cs_s, in_path, ref_path)) {
        free(input);
        return;
    }
    size_t ref_count = 0;
    double* ref = read_raw(ref_path, &ref_count);
    ASSERT_TRUE(ref != NULL);
    ASSERT_EQ(NBR_FRAMES, ref_count);

    compressor_parameters_t params = {0};
    params.channels = 1;
    params.attack = attack;
    params.release = release;
    params.threshold = threshold;
    params.factor = factor;
    params.makeup_gain = makeup_gain;
    params.has_makeup_gain = true;
    params.soft_clip = true;
    params.clip_limit = clip_limit;
    params.has_clip_limit = true;

    compressor_processor_t* comp = compressor_processor_create("compressor", &params, SAMPLE_RATE, NBR_FRAMES);
    ASSERT_TRUE(comp != NULL);

    audio_chunk_t* chunk = audio_chunk_create(NBR_FRAMES, 1);
    double* ch0 = audio_chunk_get_channel(chunk, 0);
    memcpy(ch0, input, NBR_FRAMES * sizeof(double));
    chunk->valid_frames = NBR_FRAMES;

    compressor_processor_process(comp, chunk);

    ASSERT_EQ(ref_count, chunk->valid_frames);
    double max_abs_diff = 0.0;
    for (size_t i = 0; i < ref_count; i++) {
        double d = fabs(ch0[i] - ref[i]);
        if (d > max_abs_diff) max_abs_diff = d;
    }
    printf("[compressor] maxAbsDiff=%.3e\n", max_abs_diff);
    ASSERT_TRUE(max_abs_diff < 1e-12);

    audio_chunk_free(chunk);
    compressor_processor_free(comp);
    free(input);
    free(ref);
}

TEST(NoiseGate_Vs_RustReference) {
    const char* label = "noisegate-compare";
    double* input = (double*)malloc(NBR_FRAMES * sizeof(double));
    make_test_signal(input, NBR_FRAMES);
    char in_path[256], ref_path[256];
    snprintf(in_path, sizeof(in_path), "/tmp/cdsp_gate_%s_in.raw", label);
    snprintf(ref_path, sizeof(ref_path), "/tmp/cdsp_gate_%s_ref.raw", label);
    write_raw(input, NBR_FRAMES, in_path);

    double attack = 0.005, release = 0.05, threshold = -24.0, attenuation = 20.0;
    char att_s[64], rel_s[64], thr_s[64], atten_s[64], sr_s[64], cs_s[64];
    snprintf(att_s, sizeof(att_s), "%.17g", attack);
    snprintf(rel_s, sizeof(rel_s), "%.17g", release);
    snprintf(thr_s, sizeof(thr_s), "%.17g", threshold);
    snprintf(atten_s, sizeof(atten_s), "%.17g", attenuation);
    snprintf(sr_s, sizeof(sr_s), "%d", SAMPLE_RATE);
    snprintf(cs_s, sizeof(cs_s), "%d", CHUNK_SIZE);

    if (!RUN_HARNESS("noisegate", att_s, rel_s, thr_s, atten_s, sr_s, cs_s, in_path, ref_path)) {
        free(input);
        return;
    }
    size_t ref_count = 0;
    double* ref = read_raw(ref_path, &ref_count);
    ASSERT_TRUE(ref != NULL);
    ASSERT_EQ(NBR_FRAMES, ref_count);

    noise_gate_parameters_t params = {0};
    params.channels = 1;
    params.attack = attack;
    params.release = release;
    params.threshold = threshold;
    params.attenuation = attenuation;

    noise_gate_processor_t* gate = noise_gate_processor_create("noisegate", &params, SAMPLE_RATE, NBR_FRAMES);
    ASSERT_TRUE(gate != NULL);

    audio_chunk_t* chunk = audio_chunk_create(NBR_FRAMES, 1);
    double* ch0 = audio_chunk_get_channel(chunk, 0);
    memcpy(ch0, input, NBR_FRAMES * sizeof(double));
    chunk->valid_frames = NBR_FRAMES;

    noise_gate_processor_process(gate, chunk);

    ASSERT_EQ(ref_count, chunk->valid_frames);
    double max_abs_diff = 0.0;
    for (size_t i = 0; i < ref_count; i++) {
        double d = fabs(ch0[i] - ref[i]);
        if (d > max_abs_diff) max_abs_diff = d;
    }
    printf("[noisegate] maxAbsDiff=%.3e\n", max_abs_diff);
    ASSERT_TRUE(max_abs_diff < 1e-12);

    audio_chunk_free(chunk);
    noise_gate_processor_free(gate);
    free(input);
    free(ref);
}

TEST(RACE_Vs_RustReference) {
    const char* label = "race-compare";
    double* input0 = (double*)malloc(NBR_FRAMES * sizeof(double));
    double* input1 = (double*)malloc(NBR_FRAMES * sizeof(double));
    make_test_signal(input0, NBR_FRAMES);
    for (size_t i = 0; i < NBR_FRAMES; i++) {
        input1[i] = input0[i] * 0.5;
    }

    char in_path0[256], in_path1[256], ref_path0[256], ref_path1[256];
    snprintf(in_path0, sizeof(in_path0), "/tmp/cdsp_race_%s_in0.raw", label);
    snprintf(in_path1, sizeof(in_path1), "/tmp/cdsp_race_%s_in1.raw", label);
    snprintf(ref_path0, sizeof(ref_path0), "/tmp/cdsp_race_%s_ref0.raw", label);
    snprintf(ref_path1, sizeof(ref_path1), "/tmp/cdsp_race_%s_ref1.raw", label);

    write_raw(input0, NBR_FRAMES, in_path0);
    write_raw(input1, NBR_FRAMES, in_path1);

    double delay = 12.0, attenuation = 8.5;
    char del_s[64], atten_s[64], sr_s[64], cs_s[64];
    snprintf(del_s, sizeof(del_s), "%.17g", delay);
    snprintf(atten_s, sizeof(atten_s), "%.17g", attenuation);
    snprintf(sr_s, sizeof(sr_s), "%d", SAMPLE_RATE);
    snprintf(cs_s, sizeof(cs_s), "%d", CHUNK_SIZE);

    if (!RUN_HARNESS("race", "0", "1", del_s, "samples", "0", atten_s, sr_s, cs_s, in_path0, in_path1, ref_path0, ref_path1)) {
        free(input0);
        free(input1);
        return;
    }

    size_t ref_count0 = 0, ref_count1 = 0;
    double* ref0 = read_raw(ref_path0, &ref_count0);
    double* ref1 = read_raw(ref_path1, &ref_count1);
    ASSERT_TRUE(ref0 != NULL);
    ASSERT_TRUE(ref1 != NULL);
    ASSERT_EQ(NBR_FRAMES, ref_count0);
    ASSERT_EQ(NBR_FRAMES, ref_count1);

    race_parameters_t params = {0};
    params.channels = 2;
    params.channel_a = 0;
    params.channel_b = 1;
    params.delay = delay;
    params.subsample_delay = false;
    params.has_subsample_delay = true;
    params.delay_unit = DELAY_UNIT_SAMPLES;
    params.has_delay_unit = true;
    params.attenuation = attenuation;

    race_processor_t* race = race_processor_create("race", &params, SAMPLE_RATE);
    ASSERT_TRUE(race != NULL);

    audio_chunk_t* chunk = audio_chunk_create(NBR_FRAMES, 2);
    double* ch0 = audio_chunk_get_channel(chunk, 0);
    double* ch1 = audio_chunk_get_channel(chunk, 1);
    memcpy(ch0, input0, NBR_FRAMES * sizeof(double));
    memcpy(ch1, input1, NBR_FRAMES * sizeof(double));
    chunk->valid_frames = NBR_FRAMES;

    race_processor_process(race, chunk);

    ASSERT_EQ(ref_count0, chunk->valid_frames);
    double max_abs_diff0 = 0.0, max_abs_diff1 = 0.0;
    for (size_t i = 0; i < ref_count0; i++) {
        double d0 = fabs(ch0[i] - ref0[i]);
        if (d0 > max_abs_diff0) max_abs_diff0 = d0;
        double d1 = fabs(ch1[i] - ref1[i]);
        if (d1 > max_abs_diff1) max_abs_diff1 = d1;
    }
    printf("[race] maxAbsDiff ch0=%.3e ch1=%.3e\n", max_abs_diff0, max_abs_diff1);
    ASSERT_TRUE(max_abs_diff0 < 1e-12);
    ASSERT_TRUE(max_abs_diff1 < 1e-12);

    audio_chunk_free(chunk);
    race_processor_free(race);
    free(input0);
    free(input1);
    free(ref0);
    free(ref1);
}

TEST_MAIN()
