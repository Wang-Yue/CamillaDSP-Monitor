#include "test_support.h"
#include "../../Sources/CDSP/DoP/dop_decoder.h"
#include "../../Sources/CDSP/DoP/dop_encoder.h"
#include <math.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

TEST(DoPEncoder_Benchmark) {
    double carrier_rate = 768000.0;
    dop_encoder_t* encoder = dop_encoder_create(2, carrier_rate, true, SDM_FILTER_SDM6, 20000.0);
    ASSERT_TRUE(encoder != NULL);
    ASSERT_TRUE(encoder->enabled);

    int frames = 1024;
    int channels = 2;
    audio_chunk_t* pcm_source = audio_chunk_create(frames, channels);
    double amplitude = 0.5;
    for (int ch = 0; ch < channels; ch++) {
        for (int t = 0; t < frames; t++) {
            audio_chunk_get_channel(pcm_source, ch)[t] = amplitude * sin(2.0 * M_PI * 1000.0 * (double)t / carrier_rate);
        }
    }

    audio_chunk_t* temp_chunk = audio_chunk_create(frames, channels);

    for (int i = 0; i < 100; i++) {
        for (int ch = 0; ch < channels; ch++) {
            memcpy(audio_chunk_get_channel(temp_chunk, ch), audio_chunk_get_channel(pcm_source, ch), frames * sizeof(double));
        }
        dop_encoder_encode(encoder, temp_chunk);
    }

    int iters = 2000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < iters; i++) {
        for (int ch = 0; ch < channels; ch++) {
            memcpy(audio_chunk_get_channel(temp_chunk, ch), audio_chunk_get_channel(pcm_source, ch), frames * sizeof(double));
        }
        dop_encoder_encode(encoder, temp_chunk);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_ns = (double)(end.tv_sec - start.tv_sec) * 1e9 + (double)(end.tv_nsec - start.tv_nsec);
    double ns_per_frame = elapsed_ns / (double)(frames * iters);
    double real_time_ratio = (1.0 / (carrier_rate * 1e-9)) / ns_per_frame;

    printf("=== DoP Encoder Throughput ===\n");
    printf("Throughput: %8.2f ns/frame\n", ns_per_frame);
    printf("Real-time ratio: %8.2fx\n", real_time_ratio);

    audio_chunk_free(temp_chunk);
    audio_chunk_free(pcm_source);
    dop_encoder_free(encoder);
}

TEST(DoPDecoder_Benchmark) {
    double carrier_rate = 768000.0;
    dop_encoder_t* encoder = dop_encoder_create(2, carrier_rate, true, SDM_FILTER_SDM6, 20000.0);
    dop_decoder_t* decoder = dop_decoder_create(2, carrier_rate, false, 20000.0);
    ASSERT_TRUE(encoder != NULL);
    ASSERT_TRUE(encoder->enabled);
    ASSERT_TRUE(decoder != NULL);

    int frames = 1024;
    int channels = 2;
    audio_chunk_t* pcm_source = audio_chunk_create(frames, channels);
    double amplitude = 0.5;
    for (int ch = 0; ch < channels; ch++) {
        for (int t = 0; t < frames; t++) {
            audio_chunk_get_channel(pcm_source, ch)[t] = amplitude * sin(2.0 * M_PI * 1000.0 * (double)t / carrier_rate);
        }
    }

    audio_chunk_t* encoded_source = audio_chunk_create(frames, channels);
    for (int ch = 0; ch < channels; ch++) {
        memcpy(audio_chunk_get_channel(encoded_source, ch), audio_chunk_get_channel(pcm_source, ch), frames * sizeof(double));
    }
    dop_encoder_encode(encoder, encoded_source);

    audio_chunk_t* temp_chunk = audio_chunk_create(frames, channels);

    for (int i = 0; i < 100; i++) {
        for (int ch = 0; ch < channels; ch++) {
            memcpy(audio_chunk_get_channel(temp_chunk, ch), audio_chunk_get_channel(encoded_source, ch), frames * sizeof(double));
        }
        bool processed = dop_decoder_detect_and_process(decoder, temp_chunk);
        ASSERT_TRUE(processed);
    }
    ASSERT_TRUE(decoder->is_dop_active);

    int iters = 2000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < iters; i++) {
        for (int ch = 0; ch < channels; ch++) {
            memcpy(audio_chunk_get_channel(temp_chunk, ch), audio_chunk_get_channel(encoded_source, ch), frames * sizeof(double));
        }
        dop_decoder_detect_and_process(decoder, temp_chunk);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_ns = (double)(end.tv_sec - start.tv_sec) * 1e9 + (double)(end.tv_nsec - start.tv_nsec);
    double ns_per_frame = elapsed_ns / (double)(frames * iters);
    double real_time_ratio = (1.0 / (carrier_rate * 1e-9)) / ns_per_frame;

    printf("=== DoP Decoder Throughput ===\n");
    printf("Throughput: %8.2f ns/frame\n", ns_per_frame);
    printf("Real-time ratio: %8.2fx\n", real_time_ratio);

    audio_chunk_free(temp_chunk);
    audio_chunk_free(encoded_source);
    audio_chunk_free(pcm_source);
    dop_decoder_free(decoder);
    dop_encoder_free(encoder);
}

TEST_MAIN()
