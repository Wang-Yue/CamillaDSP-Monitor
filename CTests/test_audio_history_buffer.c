#include "../CTests/test_support.h"
#include "../CLib/Audio/audio_history_buffer.h"
#include "../CLib/Audio/audio_chunk.h"
#include <stdlib.h>
#include <math.h>

TEST(Reset) {
    audio_history_buffer_t* buffer = audio_history_buffer_create();
    ASSERT_TRUE(buffer != NULL);
    ASSERT_EQ(0, buffer->channels);
    ASSERT_FALSE(audio_history_buffer_has_data(buffer));

    audio_history_buffer_reset(buffer, 2);
    ASSERT_EQ(2, buffer->channels);
    ASSERT_FALSE(audio_history_buffer_has_data(buffer));
    audio_history_buffer_free(buffer);
}

TEST(AppendAndRead) {
    audio_history_buffer_t* buffer = audio_history_buffer_create();
    audio_history_buffer_reset(buffer, 2);

    audio_chunk_t* chunk = audio_chunk_create(1024, 2);
    for (size_t t = 0; t < 1024; t++) {
        audio_chunk_get_channel(chunk, 0)[t] = (double)t;
        audio_chunk_get_channel(chunk, 1)[t] = (double)(t * 2);
    }
    chunk->valid_frames = 1024;

    audio_history_buffer_append(buffer, chunk);
    ASSERT_TRUE(audio_history_buffer_has_data(buffer));

    float* dest = (float*)calloc(1024, sizeof(float));
    bool enough_data = false;

    // Read channel 0
    audio_history_buffer_status_t status0 = audio_history_buffer_read_latest(buffer, dest, 1024, 0, &enough_data);
    ASSERT_EQ(AUDIO_HISTORY_BUFFER_OK, status0);
    ASSERT_TRUE(enough_data);
    ASSERT_FLOAT_EQ(0.0f, dest[0]);
    ASSERT_FLOAT_EQ(1023.0f, dest[1023]);

    // Read channel 1
    audio_history_buffer_status_t status1 = audio_history_buffer_read_latest(buffer, dest, 1024, 1, &enough_data);
    ASSERT_EQ(AUDIO_HISTORY_BUFFER_OK, status1);
    ASSERT_TRUE(enough_data);
    ASSERT_FLOAT_EQ(0.0f, dest[0]);
    ASSERT_FLOAT_EQ(2046.0f, dest[1023]);

    free(dest);
    audio_chunk_free(chunk);
    audio_history_buffer_free(buffer);
}

TEST(ReadLatestAverageChannels) {
    audio_history_buffer_t* buffer = audio_history_buffer_create();
    audio_history_buffer_reset(buffer, 2);

    audio_chunk_t* chunk = audio_chunk_create(1024, 2);
    for (size_t t = 0; t < 1024; t++) {
        audio_chunk_get_channel(chunk, 0)[t] = 1.0;
        audio_chunk_get_channel(chunk, 1)[t] = 3.0;
    }
    chunk->valid_frames = 1024;

    audio_history_buffer_append(buffer, chunk);

    float* dest = (float*)calloc(1024, sizeof(float));
    bool enough_data = false;

    audio_history_buffer_status_t status = audio_history_buffer_read_latest(buffer, dest, 1024, -1, &enough_data);
    ASSERT_EQ(AUDIO_HISTORY_BUFFER_OK, status);
    ASSERT_TRUE(enough_data);
    ASSERT_NEAR(2.0f, dest[0], 1e-5);
    ASSERT_NEAR(2.0f, dest[1023], 1e-5);

    free(dest);
    audio_chunk_free(chunk);
    audio_history_buffer_free(buffer);
}

TEST(ReadLatestEmpty) {
    audio_history_buffer_t* buffer = audio_history_buffer_create();
    float* dest = (float*)calloc(1024, sizeof(float));
    bool enough_data = false;

    audio_history_buffer_status_t status = audio_history_buffer_read_latest(buffer, dest, 1024, -1, &enough_data);
    ASSERT_EQ(AUDIO_HISTORY_BUFFER_ERROR_EMPTY, status);

    free(dest);
    audio_history_buffer_free(buffer);
}

TEST(ReadLatestChannelOutOfRange) {
    audio_history_buffer_t* buffer = audio_history_buffer_create();
    audio_history_buffer_reset(buffer, 2);
    float* dest = (float*)calloc(1024, sizeof(float));
    bool enough_data = false;

    audio_history_buffer_status_t status = audio_history_buffer_read_latest(buffer, dest, 1024, 2, &enough_data);
    ASSERT_EQ(AUDIO_HISTORY_BUFFER_ERROR_OUT_OF_RANGE, status);

    free(dest);
    audio_history_buffer_free(buffer);
}

TEST(AppendMismatchedChannels) {
    audio_history_buffer_t* buffer = audio_history_buffer_create();
    audio_history_buffer_reset(buffer, 2);

    audio_chunk_t* chunk = audio_chunk_create(1024, 1);
    chunk->valid_frames = 1024;
    audio_history_buffer_append(buffer, chunk);
    ASSERT_FALSE(audio_history_buffer_has_data(buffer));

    audio_chunk_free(chunk);
    audio_history_buffer_free(buffer);
}

TEST_MAIN()
