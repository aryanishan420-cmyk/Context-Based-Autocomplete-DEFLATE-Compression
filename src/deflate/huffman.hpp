// Canonical Huffman code construction and decoding for DEFLATE.
//
// DEFLATE caps code lengths at 15 bits, so the encoder uses the package-merge
// algorithm (Larmore & Hirschberg) to build *length-limited optimal* codes
// rather than a plain Huffman tree that might exceed the limit.
#pragma once

#include <cstdint>
#include <vector>

#include "deflate/bitio.hpp"

namespace deflate {

constexpr int kMaxCodeLen = 15;

// Encoder-side table: canonical code + bit length per symbol (len 0 = unused).
struct HuffEncoder {
  std::vector<uint32_t> code;
  std::vector<uint8_t> length;

  size_t size() const { return length.size(); }
};

// Decoder-side table in the counts/offsets form used by RFC 1951's reference
// decoder: `count[l]` codes of length l, `symbol` lists symbols in canonical
// order.
struct HuffDecoder {
  std::vector<uint16_t> count;   // index by bit length, 0..kMaxCodeLen
  std::vector<uint16_t> symbol;  // symbols sorted by (length, symbol)

  // True when the code space is exactly filled.  DEFLATE allows exactly one
  // incomplete case: a distance tree holding a single code.
  bool complete = false;
};

// Optimal code lengths for `freq`, with every length <= `limit`.
// Symbols with zero frequency get length 0.  Runs package-merge.
std::vector<uint8_t> packageMerge(const std::vector<uint64_t>& freq, int limit);

// Assigns canonical codes to the given bit lengths (RFC 1951, section 3.2.2).
HuffEncoder buildEncoder(const std::vector<uint8_t>& lengths);

// Builds a decoding table from bit lengths.  Throws on an over-subscribed or
// (unless `allow_incomplete`) an under-subscribed code.
HuffDecoder buildDecoder(const std::vector<uint8_t>& lengths,
                         bool allow_incomplete = false);

// Reads one symbol from the stream.
int decodeSymbol(BitReader& in, const HuffDecoder& dec);

// Total bits needed to emit every symbol with the given lengths.
uint64_t encodedBits(const std::vector<uint64_t>& freq,
                     const std::vector<uint8_t>& lengths);

}  // namespace deflate
