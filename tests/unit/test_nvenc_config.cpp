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

TEST(NvencConfigTest, ComputesVbvFromCappedBitrate) {
  nvenc::nvenc_config config;
  config.vbv_milliseconds = 500;

  // 5000 kbps for half a second is 2500000 bits, well above the 83333 bit single frame.
  EXPECT_EQ(2500000u, nvenc::vbv_buffer_size_bits(config, 60, nvenc::effective_bitrate_kbps(20000, 5000)));
}

TEST(NvencConfigTest, ComputesSingleFrameVbvByDefault) {
  nvenc::nvenc_config config;

  // 20000 kbps at 60 fps is 333333 bits per frame.
  EXPECT_EQ(333333u, nvenc::vbv_buffer_size_bits(config, 60, 20000));
}

TEST(NvencConfigTest, ExtendsVbvToRequestedMilliseconds) {
  nvenc::nvenc_config config;
  config.vbv_milliseconds = 500;

  // 10000 kbps for half a second is 5000000 bits, well above one frame.
  EXPECT_EQ(5000000u, nvenc::vbv_buffer_size_bits(config, 60, 10000));
}

TEST(NvencConfigTest, NeverShrinksVbvBelowASingleFrame) {
  nvenc::nvenc_config config;
  config.vbv_milliseconds = 5;

  const auto single_frame = nvenc::vbv_buffer_size_bits(nvenc::nvenc_config {}, 30, 10000);
  EXPECT_EQ(single_frame, nvenc::vbv_buffer_size_bits(config, 30, 10000));
}

TEST(NvencConfigTest, KeepsTheSingleFrameSizeWhenNoWindowIsRequested) {
  nvenc::nvenc_config config;

  // 300000 kbps at 30 fps is a 10 Mbit frame.
  EXPECT_EQ(10000000u, nvenc::vbv_buffer_size_bits(config, 30, 300000));
}

TEST(NvencConfigTest, ExtendsVbvToTheRequestedWindow) {
  nvenc::nvenc_config config;
  config.vbv_milliseconds = 1000;

  // 30000 kbps for a full second is 30000000 bits.
  EXPECT_EQ(30000000u, nvenc::vbv_buffer_size_bits(config, 60, 30000));
}

TEST(NvencConfigTest, ExtendsVbvBeyondASingleLargeFrame) {
  nvenc::nvenc_config config;
  config.vbv_milliseconds = 500;

  // 300000 kbps at 30 fps is a 10 Mbit frame, half a second is 150000000 bits.
  EXPECT_EQ(150000000u, nvenc::vbv_buffer_size_bits(config, 30, 300000));
}

TEST(NvencConfigTest, ReportsNoBufferForDegenerateBitrateOrFramerate) {
  nvenc::nvenc_config config;

  EXPECT_EQ(0u, nvenc::vbv_buffer_size_bits(config, 0, 20000));
  EXPECT_EQ(0u, nvenc::vbv_buffer_size_bits(config, 60, 0));
  EXPECT_EQ(0u, nvenc::vbv_buffer_size_bits(config, -1, -1));
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
