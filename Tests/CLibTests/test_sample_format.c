#include "../../Sources/CDSP/Config/engine_config_types.h"
#include "test_support.h"

#if defined(ENABLE_COREAUDIO)

TEST(CanonicalRawValues) {
  ASSERT_STR_EQ("S16",
                coreaudio_sample_format_to_string(COREAUDIO_SAMPLE_FORMAT_S16));
  ASSERT_STR_EQ("S24",
                coreaudio_sample_format_to_string(COREAUDIO_SAMPLE_FORMAT_S24));
  ASSERT_STR_EQ("S32",
                coreaudio_sample_format_to_string(COREAUDIO_SAMPLE_FORMAT_S32));
  ASSERT_STR_EQ("F32",
                coreaudio_sample_format_to_string(COREAUDIO_SAMPLE_FORMAT_F32));
}

TEST(DecodesCanonicalNames) {
  ASSERT_EQ(COREAUDIO_SAMPLE_FORMAT_S16,
            coreaudio_sample_format_from_string("S16"));
  ASSERT_EQ(COREAUDIO_SAMPLE_FORMAT_S24,
            coreaudio_sample_format_from_string("S24"));
  ASSERT_EQ(COREAUDIO_SAMPLE_FORMAT_S32,
            coreaudio_sample_format_from_string("S32"));
  ASSERT_EQ(COREAUDIO_SAMPLE_FORMAT_F32,
            coreaudio_sample_format_from_string("F32"));
}

TEST(RejectsAliases) {
  const char* aliases[] = {"S16LE",  "S24LE",  "S32LE",     "FLOAT32LE",
                           "F32_LE", "S16_LE", "FLOAT64LE", "s16"};
  for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
    ASSERT_EQ(COREAUDIO_SAMPLE_FORMAT_INVALID,
              coreaudio_sample_format_from_string(aliases[i]));
  }
}

TEST(AllCases) {
  int count = 0;
  for (int i = 0; i < 10; i++) {
    if (coreaudio_sample_format_to_string((coreaudio_sample_format_t)i) !=
            NULL &&
        strcmp(coreaudio_sample_format_to_string((coreaudio_sample_format_t)i),
               "Invalid") != 0) {
      count++;
    }
  }
  ASSERT_EQ(4, count);
}

#elif defined(ENABLE_ALSA)

TEST(CanonicalRawValues) {
  ASSERT_STR_EQ("S16_LE",
                alsa_sample_format_to_string(ALSA_SAMPLE_FORMAT_S16_LE));
  ASSERT_STR_EQ("S24_3_LE",
                alsa_sample_format_to_string(ALSA_SAMPLE_FORMAT_S24_3_LE));
  ASSERT_STR_EQ("S24_4_LE",
                alsa_sample_format_to_string(ALSA_SAMPLE_FORMAT_S24_4_LE));
  ASSERT_STR_EQ("S32_LE",
                alsa_sample_format_to_string(ALSA_SAMPLE_FORMAT_S32_LE));
  ASSERT_STR_EQ("F32_LE",
                alsa_sample_format_to_string(ALSA_SAMPLE_FORMAT_F32_LE));
  ASSERT_STR_EQ("F64_LE",
                alsa_sample_format_to_string(ALSA_SAMPLE_FORMAT_F64_LE));
}

TEST(DecodesCanonicalNames) {
  ASSERT_EQ(ALSA_SAMPLE_FORMAT_S16_LE,
            alsa_sample_format_from_string("S16_LE"));
  ASSERT_EQ(ALSA_SAMPLE_FORMAT_S24_3_LE,
            alsa_sample_format_from_string("S24_3_LE"));
  ASSERT_EQ(ALSA_SAMPLE_FORMAT_S24_4_LE,
            alsa_sample_format_from_string("S24_4_LE"));
  ASSERT_EQ(ALSA_SAMPLE_FORMAT_S32_LE,
            alsa_sample_format_from_string("S32_LE"));
  ASSERT_EQ(ALSA_SAMPLE_FORMAT_F32_LE,
            alsa_sample_format_from_string("F32_LE"));
  ASSERT_EQ(ALSA_SAMPLE_FORMAT_F64_LE,
            alsa_sample_format_from_string("F64_LE"));
}

TEST(RejectsAliases) {
  const char* aliases[] = {"S16", "S24",   "S32",   "FLOAT32",
                           "F32", "S16LE", "s16_le"};
  for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
    ASSERT_EQ(ALSA_SAMPLE_FORMAT_INVALID,
              alsa_sample_format_from_string(aliases[i]));
  }
}

TEST(AllCases) {
  int count = 0;
  for (int i = 0; i < 15; i++) {
    if (alsa_sample_format_to_string((alsa_sample_format_t)i) != NULL &&
        strcmp(alsa_sample_format_to_string((alsa_sample_format_t)i),
               "Invalid") != 0) {
      count++;
    }
  }
  ASSERT_EQ(6, count);
}

#endif

TEST_MAIN()
