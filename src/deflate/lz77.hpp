// Sliding-window Lempel-Ziv 1977 parser.
//
// Produces the literal / (length, distance) token stream that the Huffman
// stage of DEFLATE consumes.  Matches are found with hash chains over 3-byte
// prefixes, with optional lazy matching (defer a match by one byte if the next
// position starts a longer one), which is what gives levels 4-9 their edge.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace deflate {

constexpr size_t kWindowSize = 32768;  // RFC 1951 maximum distance
constexpr int kMinMatch = 3;
constexpr int kMaxMatch = 258;

// dist == 0 marks a literal byte; otherwise (len, dist) is a back-reference.
struct Token {
  uint16_t dist;
  uint16_t len;
  uint8_t lit;

  static Token literal(uint8_t byte) { return Token{0, 0, byte}; }
  static Token match(uint16_t length, uint16_t distance) {
    return Token{distance, length, 0};
  }
  bool isMatch() const { return dist != 0; }
};

// Effort knobs, mirroring the shape of zlib's level table.
struct Lz77Params {
  int max_chain;    // hash-chain positions examined per match search
  int nice_length;  // stop searching once a match this long is found
  int max_lazy;     // do not attempt a lazy match beyond this length
  bool lazy;        // enable one-byte lookahead

  static Lz77Params forLevel(int level);
};

struct Lz77Stats {
  size_t literals = 0;
  size_t matches = 0;
  size_t matched_bytes = 0;
  size_t chain_steps = 0;  // total hash-chain nodes visited
};

// Parses `data` into tokens.  Deterministic: same input and params, same output.
std::vector<Token> lz77Parse(const uint8_t* data, size_t size,
                             const Lz77Params& params, Lz77Stats* stats = nullptr);

}  // namespace deflate
