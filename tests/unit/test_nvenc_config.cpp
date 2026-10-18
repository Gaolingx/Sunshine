/**
 * @file tests/unit/test_nvenc_config.cpp
 * @brief Test src/nvenc/nvenc_config.h.
 */
// test includes
#include "../tests_common.h"

// local includes
#include <src/nvenc/nvenc_config.h>

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

TEST(NvencConfigTest, UsesOneInputSurfaceWhenLookaheadIsDisabled) {
  nvenc::nvenc_config config;

  EXPECT_EQ(1u, nvenc::input_surface_count(config));

  config.lookahead_depth = 0;
  EXPECT_EQ(1u, nvenc::input_surface_count(config));

  config.lookahead_depth = -1;
  EXPECT_EQ(1u, nvenc::input_surface_count(config));
}

TEST(NvencConfigTest, ReservesSurfaceSlackForTheLookaheadWindow) {
  nvenc::nvenc_config config;

  for (int depth = 1; depth <= nvenc::max_lookahead_depth; ++depth) {
    config.lookahead_depth = depth;
    const auto surfaces = nvenc::input_surface_count(config);
    EXPECT_EQ(static_cast<std::size_t>(depth) + 6, surfaces);
    EXPECT_GT(surfaces, static_cast<std::size_t>(depth));
  }
}
