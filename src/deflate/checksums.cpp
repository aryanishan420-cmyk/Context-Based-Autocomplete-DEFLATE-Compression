#include "deflate/deflate.hpp"

namespace deflate {
namespace {

struct Crc32Table {
  uint32_t v[256];
  constexpr Crc32Table() : v() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      v[i] = c;
    }
  }
};
constexpr Crc32Table kCrcTable;

}  // namespace

uint32_t crc32(const uint8_t* data, size_t size, uint32_t seed) {
  uint32_t c = seed ^ 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i)
    c = kCrcTable.v[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

uint32_t adler32(const uint8_t* data, size_t size, uint32_t seed) {
  uint32_t a = seed & 0xFFFF, b = (seed >> 16) & 0xFFFF;
  const uint32_t kMod = 65521;
  size_t i = 0;
  while (i < size) {
    // 5552 is the largest run that cannot overflow the 32-bit accumulator.
    size_t chunk = (size - i < 5552) ? size - i : 5552;
    for (size_t k = 0; k < chunk; ++k) {
      a += data[i + k];
      b += a;
    }
    a %= kMod;
    b %= kMod;
    i += chunk;
  }
  return (b << 16) | a;
}

}  // namespace deflate
