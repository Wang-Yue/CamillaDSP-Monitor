#include "test_support.h"
#include "../../Sources/CDSP/Audio/audio_buffers.h"

TEST(allocates_zeroed_storage) {
    audio_buffers_t* buffers = audio_buffers_create(4, 32);
    ASSERT_TRUE(buffers != NULL);
    ASSERT_EQ(4, buffers->channels);
    ASSERT_EQ(32, buffers->capacity);
    for (size_t ch = 0; ch < 4; ch++) {
        mutable_waveform_t buf = audio_buffers_get_channel(buffers, ch);
        ASSERT_TRUE(buf != NULL);
        for (size_t i = 0; i < 32; i++) {
            ASSERT_DOUBLE_EQ(0.0, buf[i]);
        }
    }
    audio_buffers_free(buffers);
}

TEST(channel_pointers_are_stable) {
    audio_buffers_t* buffers = audio_buffers_create(2, 16);
    ASSERT_TRUE(buffers != NULL);
    mutable_waveform_t p0 = audio_buffers_get_channel(buffers, 0);
    mutable_waveform_t p1 = audio_buffers_get_channel(buffers, 1);
    ASSERT_TRUE(p1 == p0 + 16);
    ASSERT_TRUE(audio_buffers_get_channel(buffers, 0) == p0);
    ASSERT_TRUE(audio_buffers_get_channel(buffers, 1) == p1);
    audio_buffers_free(buffers);
}

TEST(writes_are_isolated_per_channel) {
    audio_buffers_t* buffers = audio_buffers_create(3, 8);
    ASSERT_TRUE(buffers != NULL);
    for (size_t ch = 0; ch < 3; ch++) {
        mutable_waveform_t buf = audio_buffers_get_channel(buffers, ch);
        for (size_t i = 0; i < 8; i++) {
            buf[i] = (double)(ch * 100 + i);
        }
    }
    for (size_t ch = 0; ch < 3; ch++) {
        mutable_waveform_t buf = audio_buffers_get_channel(buffers, ch);
        for (size_t i = 0; i < 8; i++) {
            ASSERT_DOUBLE_EQ((double)(ch * 100 + i), buf[i]);
        }
    }
    audio_buffers_free(buffers);
}

TEST(copying_init_matches_source) {
    double w0[] = {1.0, 2.0, 3.0, 4.0};
    double w1[] = {-1.0, -2.0, -3.0, -4.0};
    const double* waveforms[] = {w0, w1};
    size_t lengths[] = {4, 4};
    
    audio_buffers_t* buffers = audio_buffers_copy_from(waveforms, lengths, 2);
    ASSERT_TRUE(buffers != NULL);
    ASSERT_EQ(2, buffers->channels);
    ASSERT_TRUE(buffers->capacity >= 4);
    
    for (size_t ch = 0; ch < 2; ch++) {
        mutable_waveform_t buf = audio_buffers_get_channel(buffers, ch);
        for (size_t i = 0; i < 4; i++) {
            ASSERT_DOUBLE_EQ(waveforms[ch][i], buf[i]);
        }
    }
    audio_buffers_free(buffers);
}

TEST(copying_init_zero_pads_shorter_channels) {
    double w0[] = {1.0, 2.0, 3.0, 4.0};
    double w1[] = {9.0, 8.0};
    const double* waveforms[] = {w0, w1};
    size_t lengths[] = {4, 2};
    
    audio_buffers_t* buffers = audio_buffers_copy_from(waveforms, lengths, 2);
    ASSERT_TRUE(buffers != NULL);
    ASSERT_EQ(4, buffers->capacity);
    
    mutable_waveform_t b0 = audio_buffers_get_channel(buffers, 0);
    ASSERT_DOUBLE_EQ(1.0, b0[0]);
    ASSERT_DOUBLE_EQ(2.0, b0[1]);
    ASSERT_DOUBLE_EQ(3.0, b0[2]);
    ASSERT_DOUBLE_EQ(4.0, b0[3]);
    
    mutable_waveform_t b1 = audio_buffers_get_channel(buffers, 1);
    ASSERT_DOUBLE_EQ(9.0, b1[0]);
    ASSERT_DOUBLE_EQ(8.0, b1[1]);
    ASSERT_DOUBLE_EQ(0.0, b1[2]);
    ASSERT_DOUBLE_EQ(0.0, b1[3]);
    
    audio_buffers_free(buffers);
}

TEST(mutation_through_cached_pointer) {
    audio_buffers_t* buffers = audio_buffers_create(2, 4);
    ASSERT_TRUE(buffers != NULL);
    mutable_waveform_t cached = audio_buffers_get_channel(buffers, 0);
    audio_buffers_get_channel(buffers, 0)[2] = 42.0;
    ASSERT_DOUBLE_EQ(42.0, cached[2]);
    cached[3] = 99.0;
    ASSERT_DOUBLE_EQ(99.0, audio_buffers_get_channel(buffers, 0)[3]);
    audio_buffers_free(buffers);
}

TEST_MAIN()
