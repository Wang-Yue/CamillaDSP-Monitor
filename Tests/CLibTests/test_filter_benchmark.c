#if defined(__linux__)
#define _GNU_SOURCE
#endif
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "test_support.h"
#include "../../Sources/CDSP/Filters/convolution.h"
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CHUNK_SIZE 1024
#define SAMPLE_RATE 48000
#define NBR_FRAMES (16 * CHUNK_SIZE)
#define NUM_COEFFS 2000

typedef struct {
    uint64_t state;
} seeded_rng_t;

static inline uint64_t seeded_rng_next(seeded_rng_t* rng) {
    rng->state = rng->state * 6364136223846793005ULL + 1442695040888963407ULL;
    return rng->state;
}

static inline double seeded_rng_random_double(seeded_rng_t* rng) {
    uint64_t val = seeded_rng_next(rng);
    double u = (double)(val >> 11) * (1.0 / 9007199254740992.0); // 2^53
    return u * 2.0 - 1.0;
}

static void write_raw(const double* data, size_t count, const char* path) {
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(data, sizeof(double), count, f);
        fclose(f);
    }
}

static void make_test_signal(double* x) {
    seeded_rng_t rng = { .state = 0xCDD5AA42DEADBEEFULL };
    const double f1 = 200.0;
    const double f2 = 1500.0;
    const double f3 = 8000.0;
    for (size_t i = 0; i < NBR_FRAMES; i++) {
        double t = (double)i / (double)SAMPLE_RATE;
        x[i] = 0.4 * sin(2.0 * M_PI * f1 * t)
             + 0.3 * sin(2.0 * M_PI * f2 * t)
             + 0.2 * sin(2.0 * M_PI * f3 * t)
             + 0.05 * seeded_rng_random_double(&rng);
    }
}

static const char* get_harness_binary(void) {
    const char* env = getenv("CDSP_FILTER_BIN");
    if (env && access(env, X_OK) == 0) return env;
    if (env && strlen(env) > 0) return env;

    const char* candidates[] = {
        "../Tests/RustHarnesses/target/release/cdsp_filter_compare",
        "Tests/RustHarnesses/target/release/cdsp_filter_compare",
        "../../Tests/RustHarnesses/target/release/cdsp_filter_compare",
        "/Users/wangyue/CamillaDSP-Monitor/Tests/RustHarnesses/target/release/cdsp_filter_compare"
    };
    for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++) {
        if (access(candidates[i], X_OK) == 0) {
            return candidates[i];
        }
    }
    return "../Tests/RustHarnesses/target/release/cdsp_filter_compare";
}

static double run_harness(const char* bin, size_t chunk_size, const char* coeffs_path, const char* in_path, const char* out_path) {
    double cdsp_ns_per_frame = NAN;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "\"%s\" conv %zu \"%s\" \"%s\" \"%s\" --bench=2000 2>&1",
             bin, chunk_size, coeffs_path, in_path, out_path);

    FILE* fp = popen(cmd, "r");
    if (!fp) return cdsp_ns_per_frame;

    char output_str[8192] = {0};
    size_t out_len = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (out_len + len < sizeof(output_str) - 1) {
            memcpy(output_str + out_len, line, len);
            out_len += len;
            output_str[out_len] = '\0';
        }
    }
    int status = pclose(fp);
    if (status == 0) {
        unsigned long long ns_total = 0;
        int frames_per_iter = 0;
        int iters = 0;

        char* saveptr = NULL;
        char* token = strtok_r(output_str, " \t\r\n", &saveptr);
        while (token) {
            char* eq = strchr(token, '=');
            if (eq) {
                *eq = '\0';
                const char* k = token;
                const char* v = eq + 1;
                if (strcmp(k, "BENCH_NS_TOTAL") == 0) {
                    ns_total = strtoull(v, NULL, 10);
                } else if (strcmp(k, "BENCH_OUT_FRAMES_PER_ITER") == 0) {
                    frames_per_iter = (int)strtol(v, NULL, 10);
                } else if (strcmp(k, "BENCH_ITERS") == 0) {
                    iters = (int)strtol(v, NULL, 10);
                }
            }
            token = strtok_r(NULL, " \t\r\n", &saveptr);
        }
        if (ns_total > 0 && frames_per_iter > 0 && iters > 0) {
            cdsp_ns_per_frame = (double)ns_total / (double)((long long)frames_per_iter * iters);
        }
    } else {
        printf("⚠️ CamillaDSP harness failed with status %d: %s\n", status, output_str);
    }
    return cdsp_ns_per_frame;
}

TEST(Convolution_Benchmark) {
    const char* label = "conv-bench";
    double* input = (double*)malloc(NBR_FRAMES * sizeof(double));
    ASSERT_TRUE(input != NULL);
    make_test_signal(input);

    char in_path[256], out_path[256], coeffs_path[256];
    snprintf(in_path, sizeof(in_path), "/tmp/cdsp_conv_%s_in.raw", label);
    snprintf(out_path, sizeof(out_path), "/tmp/cdsp_conv_%s_out.raw", label);
    snprintf(coeffs_path, sizeof(coeffs_path), "/tmp/cdsp_conv_%s_coeffs.raw", label);

    write_raw(input, NBR_FRAMES, in_path);

    double* coeffs = (double*)malloc(NUM_COEFFS * sizeof(double));
    ASSERT_TRUE(coeffs != NULL);
    seeded_rng_t rng = { .state = 0xBE11CULL };
    for (size_t i = 0; i < NUM_COEFFS; i++) {
        coeffs[i] = seeded_rng_random_double(&rng);
    }
    write_raw(coeffs, NUM_COEFFS, coeffs_path);

    // Measure CamillaDSP
    double cdsp_ns_per_frame = NAN;
    const char* bin = get_harness_binary();
    if (access(bin, X_OK) == 0) {
        cdsp_ns_per_frame = run_harness(bin, CHUNK_SIZE, coeffs_path, in_path, out_path);
    }

    // Measure C convolution_filter
    conv_parameters_t params = { .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = NUM_COEFFS };
    convolution_filter_t* filter = convolution_filter_create("conv-bench", &params, CHUNK_SIZE);
    ASSERT_TRUE(filter != NULL);

    double* slice = (double*)malloc(CHUNK_SIZE * sizeof(double));
    ASSERT_TRUE(slice != NULL);

    // Warm-up
    size_t idx = 0;
    while (idx < NBR_FRAMES) {
        size_t end = (idx + CHUNK_SIZE < NBR_FRAMES) ? (idx + CHUNK_SIZE) : NBR_FRAMES;
        size_t count = end - idx;
        memcpy(slice, &input[idx], count * sizeof(double));
        convolution_filter_process(filter, slice, count);
        idx = end;
    }

    int iters = 2000;
    struct timespec start, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < iters; i++) {
        idx = 0;
        while (idx < NBR_FRAMES) {
            size_t end = (idx + CHUNK_SIZE < NBR_FRAMES) ? (idx + CHUNK_SIZE) : NBR_FRAMES;
            size_t count = end - idx;
            memcpy(slice, &input[idx], count * sizeof(double));
            convolution_filter_process(filter, slice, count);
            idx = end;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end_time);

    double elapsed_ns = (double)(end_time.tv_sec - start.tv_sec) * 1e9 + (double)(end_time.tv_nsec - start.tv_nsec);
    double c_ns_per_frame = elapsed_ns / (double)(NBR_FRAMES * iters);

    printf("=== Convolution Filter Throughput ===\n");
    printf("C convolution_filter    : %8.1f ns/frame\n", c_ns_per_frame);
    printf("CamillaDSP FftConv      : %8.1f ns/frame\n", cdsp_ns_per_frame);
    double speedup = cdsp_ns_per_frame / c_ns_per_frame;
    printf("Relative Speedup        : %8.2fx\n", speedup);

    free(slice);
    convolution_filter_free(filter);
    free(coeffs);
    free(input);
}

TEST_MAIN()
