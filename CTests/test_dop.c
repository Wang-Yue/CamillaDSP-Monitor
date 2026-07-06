#include "../CTests/test_support.h"
#include "../CLib/DoP/dop_decoder.h"
#include "../CLib/DoP/dop_encoder.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

TEST(DoPDetectionAndBypass) {
    int multipliers[] = {64, 128, 256};
    double base_rates[] = {44100.0, 48000.0};

    for (int i = 0; i < 3; i++) {
        int mult = multipliers[i];
        for (int j = 0; j < 2; j++) {
            double base_rate = base_rates[j];
            double pcm_sample_rate = base_rate * (double)mult / 16.0;
            dop_decoder_t* decoder = dop_decoder_create(2, pcm_sample_rate, false, 20000.0);
            ASSERT_TRUE(decoder != NULL);

            audio_chunk_t* chunk = audio_chunk_create(64, 2);
            for (size_t t = 0; t < 64; t++) {
                uint32_t marker = (t % 2 == 0) ? 0x05 : 0xFA;
                uint32_t val24 = (marker << 16) | 0x1234;
                int32_t int_val = (int32_t)(val24 << 8) >> 8;
                double float_val = (double)int_val / 8388608.0;
                audio_chunk_get_channel(chunk, 0)[t] = float_val;
                audio_chunk_get_channel(chunk, 1)[t] = float_val;
            }

            audio_chunk_t* part_chunk = audio_chunk_create(20, 2);
            for (int ch = 0; ch < 2; ch++) {
                for (size_t t = 0; t < 20; t++) {
                    audio_chunk_get_channel(part_chunk, ch)[t] = audio_chunk_get_channel(chunk, ch)[t];
                }
            }
            bool is_decoded = dop_decoder_detect_and_process(decoder, part_chunk);
            ASSERT_FALSE(is_decoded);
            ASSERT_FALSE(decoder->is_dop_active);
            audio_chunk_free(part_chunk);

            audio_chunk_t* part_chunk2 = audio_chunk_create(44, 2);
            for (int ch = 0; ch < 2; ch++) {
                for (size_t t = 0; t < 44; t++) {
                    audio_chunk_get_channel(part_chunk2, ch)[t] = audio_chunk_get_channel(chunk, ch)[t + 20];
                }
            }
            is_decoded = dop_decoder_detect_and_process(decoder, part_chunk2);
            ASSERT_TRUE(is_decoded);
            ASSERT_TRUE(decoder->is_dop_active);
            audio_chunk_free(part_chunk2);
            dop_decoder_free(decoder);

            dop_decoder_t* bypassed_decoder = dop_decoder_create(2, pcm_sample_rate, true, 20000.0);
            audio_chunk_t* test_chunk = audio_chunk_create(64, 2);
            for (int ch = 0; ch < 2; ch++) {
                for (size_t t = 0; t < 64; t++) {
                    audio_chunk_get_channel(test_chunk, ch)[t] = audio_chunk_get_channel(chunk, ch)[t];
                }
            }
            bool processed = dop_decoder_detect_and_process(bypassed_decoder, test_chunk);
            ASSERT_FALSE(processed);
            ASSERT_FALSE(bypassed_decoder->is_dop_active);
            audio_chunk_free(test_chunk);
            audio_chunk_free(chunk);
            dop_decoder_free(bypassed_decoder);
        }
    }
}

TEST(DoPFalsePositives) {
    int multipliers[] = {64, 128, 256};
    double base_rates[] = {44100.0, 48000.0};

    for (int i = 0; i < 3; i++) {
        int mult = multipliers[i];
        for (int j = 0; j < 2; j++) {
            double base_rate = base_rates[j];
            double pcm_sample_rate = base_rate * (double)mult / 16.0;
            dop_decoder_t* decoder = dop_decoder_create(1, pcm_sample_rate, false, 20000.0);
            audio_chunk_t* chunk = audio_chunk_create(64, 1);

            for (size_t t = 0; t < 64; t++) audio_chunk_get_channel(chunk, 0)[t] = 0.0;
            bool res1 = dop_decoder_detect_and_process(decoder, chunk);
            ASSERT_FALSE(res1);

            for (size_t t = 0; t < 64; t++) {
                audio_chunk_get_channel(chunk, 0)[t] = ((double)rand() / RAND_MAX) * 2.0 - 1.0;
            }
            bool res2 = dop_decoder_detect_and_process(decoder, chunk);
            ASSERT_FALSE(res2);

            audio_chunk_free(chunk);
            dop_decoder_free(decoder);
        }
    }
}

TEST(MultiChunkDoPStreamStability) {
    int multipliers[] = {64, 128, 256};
    double base_rates[] = {44100.0, 48000.0};

    for (int i = 0; i < 3; i++) {
        int mult = multipliers[i];
        for (int j = 0; j < 2; j++) {
            double base_rate = base_rates[j];
            double pcm_sample_rate = base_rate * (double)mult / 16.0;
            dop_decoder_t* decoder = dop_decoder_create(2, pcm_sample_rate, false, 20000.0);
            int chunk_size = 1024;
            int num_chunks = 10;

            int global_frame_idx = 0;
            for (int chunk_idx = 1; chunk_idx <= num_chunks; chunk_idx++) {
                audio_chunk_t* chunk = audio_chunk_create(chunk_size, 2);
                for (int t = 0; t < chunk_size; t++) {
                    uint32_t marker = (global_frame_idx % 2 == 0) ? 0x05 : 0xFA;
                    uint32_t val24 = (marker << 16) | 0x4321;
                    int32_t int_val = (int32_t)(val24 << 8) >> 8;
                    double float_val = (double)int_val / 8388608.0;
                    audio_chunk_get_channel(chunk, 0)[t] = float_val;
                    audio_chunk_get_channel(chunk, 1)[t] = float_val;
                    global_frame_idx++;
                }

                bool processed = dop_decoder_detect_and_process(decoder, chunk);
                ASSERT_TRUE(processed);
                ASSERT_TRUE(decoder->is_dop_active);
                audio_chunk_free(chunk);
            }
            dop_decoder_free(decoder);
        }
    }
}

TEST(DoPRoundtripSINAD) {
    int multipliers[] = {64, 128, 256};
    double base_rates[] = {44100.0, 48000.0};

    for (int i = 0; i < 3; i++) {
        int mult = multipliers[i];
        for (int j = 0; j < 2; j++) {
            double base_rate = base_rates[j];
            double pcm_sample_rate = base_rate * (double)mult / 16.0;
            dop_encoder_t* encoder = dop_encoder_create(1, pcm_sample_rate, true, SDM_FILTER_SDM6, 20000.0);
            dop_decoder_t* decoder = dop_decoder_create(1, pcm_sample_rate, false, 20000.0);
            ASSERT_TRUE(encoder != NULL);
            ASSERT_TRUE(decoder != NULL);

            double frames_per_cycle = pcm_sample_rate / 1000.0;
            int active_frames = (int)round(frames_per_cycle * 10.0);
            int settle_frames = (int)round(frames_per_cycle * 4.0);
            int frames = settle_frames + active_frames;

            audio_chunk_t* chunk = audio_chunk_create(frames, 1);
            double amplitude = 0.7071;
            for (int t = 0; t < frames; t++) {
                audio_chunk_get_channel(chunk, 0)[t] = amplitude * sin(2.0 * M_PI * 1000.0 * (double)t / pcm_sample_rate);
            }

            dop_encoder_encode(encoder, chunk);

            bool processed = dop_decoder_detect_and_process(decoder, chunk);
            ASSERT_TRUE(processed);
            ASSERT_TRUE(decoder->is_dop_active);

            double target_freq = 1000.0;
            double cos_sum = 0.0;
            double sin_sum = 0.0;
            for (int t = settle_frames; t < frames; t++) {
                double angle = 2.0 * M_PI * target_freq * (double)t / pcm_sample_rate;
                cos_sum += audio_chunk_get_channel(chunk, 0)[t] * cos(angle);
                sin_sum += audio_chunk_get_channel(chunk, 0)[t] * sin(angle);
            }
            double cos_amp = (2.0 / (double)active_frames) * cos_sum;
            double sin_amp = (2.0 / (double)active_frames) * sin_sum;
            double fundamental_power = (cos_amp * cos_amp + sin_amp * sin_amp) / 2.0;

            double total_power = 0.0;
            for (int t = settle_frames; t < frames; t++) {
                total_power += audio_chunk_get_channel(chunk, 0)[t] * audio_chunk_get_channel(chunk, 0)[t];
            }
            total_power /= (double)active_frames;

            double noise_power = total_power - fundamental_power;
            if (noise_power < 1e-20) noise_power = 1e-20;
            double sinad = 10.0 * log10(fundamental_power / noise_power);

            double expected_min_sinad = 80.0;
            if (mult == 64) expected_min_sinad = 90.0;
            else if (mult == 128) expected_min_sinad = 110.0;
            else if (mult == 256) expected_min_sinad = 115.0;

            ASSERT_TRUE(sinad >= expected_min_sinad);

            audio_chunk_free(chunk);
            dop_decoder_free(decoder);
            dop_encoder_free(encoder);
        }
    }
}

TEST_MAIN()
