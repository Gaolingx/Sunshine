/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// local includes
#include <src/network.h>

namespace stream {
  std::vector<uint8_t> concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2);
  std::optional<std::pair<std::uint16_t, std::string_view>> parse_control_packet(const ENetPacket &packet);
  double packets_per_millisecond(int bitrate_kbps, int fec_percentage, int blocksize);
}  // namespace stream

TEST(PacketsPerMillisecondTests, ComputesPacingFromBitrate) {
  // 10000 kbps over 1000 byte packets is 1.25 packets per millisecond, 1.875 with 50% headroom.
  EXPECT_DOUBLE_EQ(1.875, stream::packets_per_millisecond(10000, 0, 1000));
}

TEST(PacketsPerMillisecondTests, IncludesFecOverhead) {
  // 25% FEC raises the same budget to 18750 bits per millisecond, or 2.34375 packets per millisecond.
  EXPECT_DOUBLE_EQ(2.34375, stream::packets_per_millisecond(10000, 25, 1000));
}

TEST(PacketsPerMillisecondTests, KeepsHeadroomAboveEncoderBitrate) {
  // Pacing must run ahead of the encoder bitrate, but stay close enough to bound the queue on a
  // congested link.
  const auto average_rate = static_cast<double>(10000) / (1000 * 8.0);

  EXPECT_GT(stream::packets_per_millisecond(10000, 0, 1000), average_rate);
  EXPECT_LE(stream::packets_per_millisecond(10000, 0, 1000), average_rate * 1.5);
}

TEST(PacketsPerMillisecondTests, KeepsPacingAboveZero) {
  // A tiny bitrate must not produce a zero rate, otherwise the sender would divide by zero.
  EXPECT_DOUBLE_EQ(1e-3, stream::packets_per_millisecond(1, 0, 1000));
}

TEST(PacketsPerMillisecondTests, DisablesPacingWithoutBitrateOrPacketSize) {
  const auto unlimited = static_cast<double>(std::numeric_limits<std::uint32_t>::max());

  EXPECT_DOUBLE_EQ(unlimited, stream::packets_per_millisecond(0, 20, 1000));
  EXPECT_DOUBLE_EQ(unlimited, stream::packets_per_millisecond(-1, 20, 1000));
  EXPECT_DOUBLE_EQ(unlimited, stream::packets_per_millisecond(10000, 20, 0));
}

TEST(ConcatAndInsertTests, ConcatNoInsertionTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(0, 2, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatLargeStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, sizeof(b1) + sizeof(b2) + 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatSmallStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 0, 'b', 0, 'c', 0, 'd', 0, 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ControlPacketTests, RejectsZeroLengthPacket) {
  net::packet_t packet {enet_packet_create(nullptr, 0, 0)};

  ASSERT_NE(packet, nullptr);
  EXPECT_EQ(packet->data, nullptr);
  EXPECT_EQ(stream::parse_control_packet(*packet), std::nullopt);
}

TEST(ControlPacketTests, RejectsOneBytePacket) {
  const std::uint8_t data {0x06};
  net::packet_t packet {enet_packet_create(&data, sizeof(data), 0)};

  ASSERT_NE(packet, nullptr);
  EXPECT_EQ(stream::parse_control_packet(*packet), std::nullopt);
}

TEST(ControlPacketTests, AcceptsTypeWithoutPayload) {
  const std::array<std::uint8_t, 2> data {0x06, 0x02};
  net::packet_t packet {enet_packet_create(data.data(), data.size(), 0)};

  ASSERT_NE(packet, nullptr);
  auto message = stream::parse_control_packet(*packet);
  ASSERT_TRUE(message);
  EXPECT_EQ(message->first, 0x0206);
  EXPECT_TRUE(message->second.empty());
}

TEST(ControlPacketTests, AcceptsTypeAndPayload) {
  const std::array<std::uint8_t, 5> data {0x06, 0x02, 'a', 'b', 'c'};
  net::packet_t packet {enet_packet_create(data.data(), data.size(), 0)};

  ASSERT_NE(packet, nullptr);
  auto message = stream::parse_control_packet(*packet);
  ASSERT_TRUE(message);
  EXPECT_EQ(message->first, 0x0206);
  EXPECT_EQ(message->second, "abc");
}
