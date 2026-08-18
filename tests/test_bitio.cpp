#include <random>

#include "deflate/bitio.hpp"
#include "test_framework.hpp"

using namespace deflate;

TEST(BitIO, RoundTripsRandomWidths) {
  std::mt19937 rng(4);
  std::vector<std::pair<uint32_t, int>> values;
  std::vector<uint8_t> buffer;
  BitWriter bw(buffer);
  for (int i = 0; i < 5000; ++i) {
    const int width = 1 + rng() % 24;
    const uint32_t value = rng() & ((1u << width) - 1);
    values.push_back({value, width});
    bw.putBits(value, width);
  }
  bw.flush();

  BitReader br(buffer.data(), buffer.size());
  for (const auto& [value, width] : values) CHECK_EQ(br.getBits(width), value);
}

TEST(BitIO, PacksLeastSignificantBitFirst) {
  // RFC 1951 section 3.1.1: bits fill each byte starting at its LSB.
  std::vector<uint8_t> buffer;
  BitWriter bw(buffer);
  bw.putBits(1, 1);
  bw.putBits(0, 1);
  bw.putBits(1, 1);
  bw.flush();
  CHECK_EQ(buffer.size(), 1u);
  CHECK_EQ(buffer[0], 0x05);
}

TEST(BitIO, HuffmanCodesGoMostSignificantBitFirst) {
  std::vector<uint8_t> buffer;
  BitWriter bw(buffer);
  bw.putCode(0x30, 8);  // the fixed code for literal 0
  bw.flush();
  CHECK_EQ(buffer[0], 0x0C);  // 00110000 reversed
}

TEST(BitIO, ReverseBitsIsItsOwnInverse) {
  for (int len = 1; len <= 15; ++len)
    for (uint32_t v = 0; v < (1u << len); v += 7)
      CHECK_EQ(reverseBits(reverseBits(v, len), len), v);
}

TEST(BitIO, ReadingPastTheEndThrows) {
  std::vector<uint8_t> buffer = {0xFF};
  BitReader br(buffer.data(), buffer.size());
  br.getBits(8);
  CHECK_THROWS(br.getBits(1));
}
