/**
 * @file tests/unit/test_nvenc_config.cpp
 * @brief Test src/nvenc/nvenc_config.h.
 */
// test includes
#include "../tests_common.h"

// local includes
#include <src/nvenc/nvenc_config.h>

TEST(NvencConfigTest, KeepsRequestedBitrateWithoutCeiling) {
  EXPECT_EQ(20000, nvenc::effective_bitrate_kbps(20000, 0));
  EXPECT_EQ(20000, nvenc::effective_bitrate_kbps(20000, -1));
}

TEST(NvencConfigTest, AppliesBitrateCeiling) {
  EXPECT_EQ(5000, nvenc::effective_bitrate_kbps(20000, 5000));
}

TEST(NvencConfigTest, IgnoresCeilingAboveRequestedBitrate) {
  EXPECT_EQ(20000, nvenc::effective_bitrate_kbps(20000, 50000));
}

TEST(NvencConfigTest, EnablesUnidirectionalBFramesWithTwoReferences) {
  EXPECT_TRUE(nvenc::use_unidirectional_b_frames(true, true, 2));
  EXPECT_TRUE(nvenc::use_unidirectional_b_frames(true, true, 5));
}

TEST(NvencConfigTest, KeepsUnidirectionalBFramesDisabledWhenNotRequested) {
  EXPECT_FALSE(nvenc::use_unidirectional_b_frames(false, true, 5));
}

TEST(NvencConfigTest, KeepsUnidirectionalBFramesDisabledWithoutGpuSupport) {
  EXPECT_FALSE(nvenc::use_unidirectional_b_frames(true, false, 5));
}

TEST(NvencConfigTest, KeepsUnidirectionalBFramesDisabledWithLessThanTwoReferences) {
  EXPECT_FALSE(nvenc::use_unidirectional_b_frames(true, true, 1));
  EXPECT_FALSE(nvenc::use_unidirectional_b_frames(true, true, 0));
}
