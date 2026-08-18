#include <random>
#include <string>

#include "deflate/deflate.hpp"
#include "test_framework.hpp"

using namespace deflate;

namespace {

std::vector<uint8_t> bytes(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

void roundTrip(const std::vector<uint8_t>& data, int level, Container container) {
  Options opts;
  opts.level = level;
  opts.container = container;
  const std::vector<uint8_t> encoded = compress(data, opts);
  CHECK_EQ(decompress(encoded, container), data);
}

}  // namespace

TEST(Deflate, RoundTripsAcrossLevelsAndContainers) {
  const std::vector<std::vector<uint8_t>> inputs = {
      {},
      bytes("a"),
      bytes("hello hello hello hello hello"),
      std::vector<uint8_t>(100000, 0x5A),
      bytes(std::string(70000, 'q') + "tail"),
  };
  for (const std::vector<uint8_t>& input : inputs)
    for (int level = 0; level <= 9; ++level)
      for (Container c : {Container::Raw, Container::Zlib, Container::Gzip})
        roundTrip(input, level, c);
}

TEST(Deflate, RoundTripsIncompressibleData) {
  std::mt19937 rng(5);
  std::vector<uint8_t> data(120000);
  for (uint8_t& b : data) b = static_cast<uint8_t>(rng() & 0xFF);
  roundTrip(data, 9, Container::Gzip);

  // Random data cannot be compressed, so the encoder must fall back to stored
  // blocks rather than inflating the output.
  Options opts;
  opts.level = 9;
  opts.container = Container::Raw;
  Report report;
  const std::vector<uint8_t> encoded = compress(data, opts, &report);
  CHECK(encoded.size() <= data.size() + data.size() / 1000 + 64);
  bool has_stored = false;
  for (const BlockReport& b : report.blocks)
    if (std::string(b.type) == "stored") has_stored = true;
  CHECK(has_stored);
}

TEST(Deflate, CompressesRepetitiveTextWell) {
  std::string text;
  for (int i = 0; i < 2000; ++i) text += "the rain in spain falls mainly on the plain ";
  const std::vector<uint8_t> data = bytes(text);

  Options opts;
  opts.level = 9;
  opts.container = Container::Raw;
  Report report;
  const std::vector<uint8_t> encoded = compress(data, opts, &report);
  CHECK(report.ratio() < 0.02);
  CHECK_EQ(decompress(encoded, Container::Raw), data);
}

TEST(Deflate, HigherLevelsNeverCompressWorse) {
  std::string text;
  for (int i = 0; i < 500; ++i)
    text += "context based autocomplete and compression algorithms ";
  const std::vector<uint8_t> data = bytes(text);

  size_t previous = SIZE_MAX;
  for (int level = 1; level <= 9; ++level) {
    Options opts;
    opts.level = level;
    opts.container = Container::Raw;
    const size_t size = compress(data, opts).size();
    CHECK(size <= previous);
    previous = size;
  }
}

TEST(Deflate, SmallBlocksStillRoundTrip) {
  std::string text;
  for (int i = 0; i < 300; ++i) text += "block splitting boundary test ";
  const std::vector<uint8_t> data = bytes(text);

  Options opts;
  opts.level = 6;
  opts.container = Container::Zlib;
  opts.block_tokens = 17;  // forces many tiny blocks
  Report report;
  const std::vector<uint8_t> encoded = compress(data, opts, &report);
  CHECK(report.blocks.size() >= 3);
  // Every block but the last is filled to the token limit.
  for (size_t i = 0; i + 1 < report.blocks.size(); ++i)
    CHECK_EQ(report.blocks[i].tokens, opts.block_tokens);
  CHECK(report.blocks.back().tokens <= opts.block_tokens);
  CHECK_EQ(decompress(encoded, Container::Zlib), data);
}

TEST(Deflate, ContainerDetectionWorks) {
  const std::vector<uint8_t> data = bytes("detect me");
  for (Container c : {Container::Zlib, Container::Gzip}) {
    Options opts;
    opts.container = c;
    const std::vector<uint8_t> encoded = compress(data, opts);
    CHECK(detectContainer(encoded.data(), encoded.size()) == c);
  }
}

TEST(Deflate, ChecksumsMatchKnownValues) {
  const std::vector<uint8_t> data = bytes("The quick brown fox jumps over the lazy dog");
  CHECK_EQ(crc32(data.data(), data.size()), 0x414FA339u);
  CHECK_EQ(adler32(data.data(), data.size()), 0x5BDC0FDAu);
}

TEST(Deflate, CorruptStreamsAreRejected) {
  const std::vector<uint8_t> data = bytes(std::string(5000, 'z') + "payload");
  Options opts;
  opts.container = Container::Gzip;
  std::vector<uint8_t> encoded = compress(data, opts);

  std::vector<uint8_t> flipped_payload = encoded;
  flipped_payload[flipped_payload.size() - 10] ^= 0xFF;  // corrupt the data
  CHECK_THROWS(decompress(flipped_payload, Container::Gzip));

  std::vector<uint8_t> flipped_crc = encoded;
  flipped_crc[flipped_crc.size() - 5] ^= 0x01;  // corrupt the stored CRC
  CHECK_THROWS(decompress(flipped_crc, Container::Gzip));

  std::vector<uint8_t> truncated(encoded.begin(), encoded.begin() + encoded.size() / 2);
  CHECK_THROWS(decompress(truncated, Container::Gzip));

  std::vector<uint8_t> bad_magic = encoded;
  bad_magic[0] = 0x00;
  CHECK_THROWS(decompress(bad_magic, Container::Gzip));
}

TEST(Deflate, DecodesAStoredBlockWrittenByHand) {
  // BFINAL=1, BTYPE=00, then LEN/NLEN and the raw bytes.
  const std::vector<uint8_t> stream = {0x01, 0x03, 0x00, 0xFC, 0xFF, 'a', 'b', 'c'};
  CHECK_EQ(decompress(stream, Container::Raw), bytes("abc"));
}

TEST(Deflate, DecodesAFixedHuffmanBlockWrittenByHand) {
  // A fixed-Huffman block holding just the end-of-block symbol.
  const std::vector<uint8_t> stream = {0x03, 0x00};
  CHECK(decompress(stream, Container::Raw).empty());
}
