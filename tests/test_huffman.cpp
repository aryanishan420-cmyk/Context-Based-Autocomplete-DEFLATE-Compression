#include <cmath>
#include <queue>
#include <random>

#include "deflate/huffman.hpp"
#include "deflate/tables.hpp"
#include "test_framework.hpp"

using namespace deflate;

namespace {

double kraftSum(const std::vector<uint8_t>& lengths) {
  double sum = 0;
  for (uint8_t l : lengths)
    if (l) sum += std::pow(2.0, -static_cast<double>(l));
  return sum;
}

// Cost of an unrestricted Huffman code, as the optimality reference.
uint64_t unlimitedHuffmanCost(const std::vector<uint64_t>& freq) {
  std::priority_queue<uint64_t, std::vector<uint64_t>, std::greater<>> pq;
  for (uint64_t f : freq)
    if (f) pq.push(f);
  if (pq.size() < 2) return pq.empty() ? 0 : pq.top();
  uint64_t cost = 0;
  while (pq.size() > 1) {
    const uint64_t a = pq.top();
    pq.pop();
    const uint64_t b = pq.top();
    pq.pop();
    cost += a + b;
    pq.push(a + b);
  }
  return cost;
}

}  // namespace

TEST(Huffman, PackageMergeProducesCompleteCode) {
  const std::vector<uint64_t> freq = {5, 9, 12, 13, 16, 45};
  const std::vector<uint8_t> lengths = packageMerge(freq, 15);
  CHECK_NEAR(kraftSum(lengths), 1.0, 1e-12);
  for (uint8_t l : lengths) CHECK(l >= 1 && l <= 15);
}

TEST(Huffman, PackageMergeMatchesTextbookExample) {
  // The classic example: the rarest symbols must end up deepest.
  const std::vector<uint64_t> freq = {45, 13, 12, 16, 9, 5};
  const std::vector<uint8_t> lengths = packageMerge(freq, 15);
  CHECK(lengths[0] < lengths[4]);
  CHECK(lengths[0] < lengths[5]);
  CHECK_EQ(encodedBits(freq, lengths), unlimitedHuffmanCost(freq));
}

TEST(Huffman, PackageMergeRespectsTheLengthLimit) {
  // Fibonacci weights are the worst case: unrestricted Huffman makes a chain
  // far deeper than 15 levels, so the limit has to bind.
  std::vector<uint64_t> freq;
  uint64_t a = 1, b = 1;
  for (int i = 0; i < 40; ++i) {
    freq.push_back(a);
    const uint64_t next = a + b;
    a = b;
    b = next;
  }
  const std::vector<uint8_t> lengths = packageMerge(freq, 15);
  for (uint8_t l : lengths) CHECK(l <= 15);
  CHECK_NEAR(kraftSum(lengths), 1.0, 1e-12);
  CHECK(encodedBits(freq, lengths) >= unlimitedHuffmanCost(freq));
}

TEST(Huffman, SingleSymbolStillGetsACode) {
  std::vector<uint64_t> freq(10, 0);
  freq[7] = 42;
  const std::vector<uint8_t> lengths = packageMerge(freq, 15);
  CHECK_EQ(lengths[7], 1);
}

TEST(Huffman, CanonicalCodesAreDecodable) {
  std::mt19937 rng(99);
  for (int trial = 0; trial < 50; ++trial) {
    std::vector<uint64_t> freq(1 + rng() % 200, 0);
    for (uint64_t& f : freq)
      if (rng() % 3) f = 1 + rng() % 5000;
    int used = 0;
    for (uint64_t f : freq) used += f > 0;
    if (used < 2) continue;

    const std::vector<uint8_t> lengths = packageMerge(freq, 15);
    const HuffEncoder enc = buildEncoder(lengths);
    const HuffDecoder dec = buildDecoder(lengths);

    // Write every used symbol, then read them back.
    std::vector<uint8_t> buffer;
    BitWriter bw(buffer);
    std::vector<int> written;
    for (size_t s = 0; s < freq.size(); ++s) {
      if (!freq[s]) continue;
      bw.putCode(enc.code[s], enc.length[s]);
      written.push_back(static_cast<int>(s));
    }
    bw.flush();

    BitReader br(buffer.data(), buffer.size());
    for (int expected : written) CHECK_EQ(decodeSymbol(br, dec), expected);
  }
}

TEST(Huffman, RejectsOverSubscribedCode) {
  // Three one-bit codes cannot exist.
  const std::vector<uint8_t> lengths = {1, 1, 1};
  CHECK_THROWS(buildDecoder(lengths));
}

TEST(Huffman, RejectsIncompleteCodeUnlessAllowed) {
  const std::vector<uint8_t> lengths = {1, 0, 0};  // half the code space unused
  CHECK_THROWS(buildDecoder(lengths));
  const HuffDecoder dec = buildDecoder(lengths, /*allow_incomplete=*/true);
  CHECK(!dec.complete);
}

TEST(Huffman, FixedTablesMatchTheSpec) {
  const std::vector<uint8_t> lit = fixedLitLenLengths();
  CHECK_EQ(lit[0], 8);
  CHECK_EQ(lit[143], 8);
  CHECK_EQ(lit[144], 9);
  CHECK_EQ(lit[255], 9);
  CHECK_EQ(lit[256], 7);
  CHECK_EQ(lit[279], 7);
  CHECK_EQ(lit[280], 8);
  CHECK_EQ(lit[287], 8);
  CHECK_NEAR(kraftSum(lit), 1.0, 1e-12);

  // RFC 1951: literal 0 is 00110000, and 144 is 110010000.
  const HuffEncoder enc = buildEncoder(lit);
  CHECK_EQ(enc.code[0], 0x30u);
  CHECK_EQ(enc.code[144], 0x190u);
}

TEST(Huffman, LengthAndDistanceTablesRoundTrip) {
  for (int len = 3; len <= 258; ++len) {
    const CodedValue c = encodeLength(len);
    CHECK(c.symbol >= 257 && c.symbol <= 285);
    const int base = kLengthBase[c.symbol - 257];
    CHECK_EQ(base + c.extra_value, len);
    CHECK(c.extra_value < (1u << c.extra_bits));
  }
  for (int dist : {1, 2, 3, 4, 5, 100, 1000, 4096, 32768}) {
    const CodedValue c = encodeDistance(dist);
    CHECK(c.symbol < kDistSymbols);
    CHECK_EQ(kDistBase[c.symbol] + c.extra_value, dist);
    CHECK(c.extra_value < (1u << c.extra_bits));
  }
}
