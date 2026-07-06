#include "test_support.h"
#include "../../Sources/CDSP/Config/engine_config_types.h"

TEST(CanonicalRawValues) {
    ASSERT_STR_EQ("S16", sample_format_to_string(SAMPLE_FORMAT_S16));
    ASSERT_STR_EQ("S24", sample_format_to_string(SAMPLE_FORMAT_S24));
    ASSERT_STR_EQ("S32", sample_format_to_string(SAMPLE_FORMAT_S32));
    ASSERT_STR_EQ("F32", sample_format_to_string(SAMPLE_FORMAT_F32));
}

TEST(DecodesCanonicalNames) {
    ASSERT_EQ(SAMPLE_FORMAT_S16, sample_format_from_string("S16"));
    ASSERT_EQ(SAMPLE_FORMAT_S24, sample_format_from_string("S24"));
    ASSERT_EQ(SAMPLE_FORMAT_S32, sample_format_from_string("S32"));
    ASSERT_EQ(SAMPLE_FORMAT_F32, sample_format_from_string("F32"));
}

TEST(RejectsAliases) {
    const char* aliases[] = {"S16LE", "S24LE", "S32LE", "FLOAT32LE", "F32_LE", "S16_LE", "FLOAT64LE", "s16"};
    for (size_t i = 0; i < sizeof(aliases)/sizeof(aliases[0]); i++) {
        ASSERT_EQ(SAMPLE_FORMAT_INVALID, sample_format_from_string(aliases[i]));
    }
}

TEST(AllCases) {
    int count = 0;
    for (int i = 0; i < 10; i++) {
        if (sample_format_to_string((sample_format_t)i) != NULL &&
            strcmp(sample_format_to_string((sample_format_t)i), "Invalid") != 0) {
            count++;
        }
    }
    ASSERT_EQ(4, count);
}

TEST_MAIN()
