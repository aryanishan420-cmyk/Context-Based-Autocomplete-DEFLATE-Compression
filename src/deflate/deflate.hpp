// DEFLATE (RFC 1951) encoder and decoder, plus the zlib (RFC 1950) and gzip
// (RFC 1952) container formats.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "deflate/lz77.hpp"

namespace deflate {

enum class Container { Raw, Zlib, Gzip };

struct Options {
  int level = 6;                  // 0 = stored only, 9 = maximum effort
  Container container = Container::Raw;
  size_t block_tokens = 16384;    // tokens per DEFLATE block
  std::string filename;           // optional FNAME field for gzip
};

struct BlockReport {
  const char* type;      // "stored", "fixed" or "dynamic"
  size_t tokens;
  size_t raw_bytes;
  uint64_t bits;
};

struct Report {
  size_t input_bytes = 0;
  size_t output_bytes = 0;
  Lz77Stats lz77;
  std::vector<BlockReport> blocks;

  double ratio() const {
    return input_bytes ? static_cast<double>(output_bytes) / input_bytes : 0.0;
  }
};

// Compresses `data`.  With Container::Gzip the result is a complete .gz file.
std::vector<uint8_t> compress(const uint8_t* data, size_t size,
                              const Options& opts, Report* report = nullptr);
std::vector<uint8_t> compress(const std::vector<uint8_t>& data,
                              const Options& opts, Report* report = nullptr);

// Decompresses a raw DEFLATE, zlib or gzip stream.  Container::Raw with an
// auto-detected header is not attempted: pass the format you expect.
// Verifies the CRC-32 / Adler-32 checksum when the container carries one.
std::vector<uint8_t> decompress(const uint8_t* data, size_t size,
                                Container container);
std::vector<uint8_t> decompress(const std::vector<uint8_t>& data,
                                Container container);

// Guesses the container from the leading bytes.
Container detectContainer(const uint8_t* data, size_t size);

uint32_t crc32(const uint8_t* data, size_t size, uint32_t seed = 0);
uint32_t adler32(const uint8_t* data, size_t size, uint32_t seed = 1);

}  // namespace deflate
