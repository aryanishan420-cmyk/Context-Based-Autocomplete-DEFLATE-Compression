#include <random>
#include <string>

#include "deflate/lz77.hpp"
#include "test_framework.hpp"

using namespace deflate;

namespace {

// Replays a token stream, which is exactly what the decoder does.
std::vector<uint8_t> replay(const std::vector<Token>& tokens) {
  std::vector<uint8_t> out;
  for (const Token& t : tokens) {
    if (!t.isMatch()) {
      out.push_back(t.lit);
      continue;
    }
    const size_t from = out.size() - t.dist;
    for (int i = 0; i < t.len; ++i) out.push_back(out[from + i]);
  }
  return out;
}

std::vector<uint8_t> bytes(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

}  // namespace

TEST(Lz77, ShortInputIsAllLiterals) {
  const std::vector<uint8_t> data = bytes("ab");
  const std::vector<Token> tokens = lz77Parse(data.data(), data.size(), Lz77Params::forLevel(6));
  CHECK_EQ(tokens.size(), 2u);
  CHECK(!tokens[0].isMatch());
}

TEST(Lz77, FindsAnObviousRepeat) {
  const std::vector<uint8_t> data = bytes("abcdefgabcdefg");
  const std::vector<Token> tokens = lz77Parse(data.data(), data.size(), Lz77Params::forLevel(9));
  bool found = false;
  for (const Token& t : tokens)
    if (t.isMatch() && t.dist == 7 && t.len == 7) found = true;
  CHECK(found);
  CHECK_EQ(replay(tokens), data);
}

TEST(Lz77, EncodesRunsAsOverlappingMatches) {
  // "aaaa..." becomes one literal plus a match at distance 1: the copy source
  // overlaps the destination, which RFC 1951 allows and relies on.
  const std::vector<uint8_t> data(300, 'a');
  const std::vector<Token> tokens = lz77Parse(data.data(), data.size(), Lz77Params::forLevel(6));
  CHECK(tokens.size() < 5);
  CHECK(tokens[1].isMatch());
  CHECK_EQ(tokens[1].dist, 1);
  CHECK_EQ(replay(tokens), data);
}

TEST(Lz77, NeverExceedsTheWindowOrMatchLimits) {
  std::mt19937 rng(11);
  std::vector<uint8_t> data(200000);
  for (uint8_t& b : data) b = static_cast<uint8_t>(rng() % 6);  // very repetitive

  for (int level : {1, 3, 6, 9}) {
    const std::vector<Token> tokens = lz77Parse(data.data(), data.size(), Lz77Params::forLevel(level));
    size_t pos = 0;
    for (const Token& t : tokens) {
      if (t.isMatch()) {
        CHECK(t.len >= kMinMatch && t.len <= kMaxMatch);
        CHECK(t.dist >= 1 && t.dist <= kWindowSize);
        CHECK(t.dist <= pos);  // never references bytes before the output
        pos += t.len;
      } else {
        pos += 1;
      }
    }
    CHECK_EQ(pos, data.size());
    CHECK_EQ(replay(tokens), data);
  }
}

TEST(Lz77, HigherLevelsSearchHarder) {
  std::string text;
  for (int i = 0; i < 400; ++i)
    text += "the quick brown fox jumps over the lazy dog ";
  const std::vector<uint8_t> data = bytes(text);

  Lz77Stats fast, slow;
  lz77Parse(data.data(), data.size(), Lz77Params::forLevel(1), &fast);
  lz77Parse(data.data(), data.size(), Lz77Params::forLevel(9), &slow);

  // Both levels cover nearly all of this very repetitive input; what a higher
  // level buys is a longer search, not necessarily more matched bytes -- lazy
  // matching deliberately emits a literal to reach a better match next.
  CHECK(slow.chain_steps > fast.chain_steps);
  CHECK(fast.matched_bytes > data.size() * 9 / 10);
  CHECK(slow.matched_bytes > data.size() * 9 / 10);
}

TEST(Lz77, LevelZeroEmitsOnlyLiterals) {
  const std::vector<uint8_t> data(1000, 'x');
  Lz77Stats stats;
  const std::vector<Token> tokens = lz77Parse(data.data(), data.size(), Lz77Params::forLevel(0), &stats);
  CHECK_EQ(tokens.size(), data.size());
  CHECK_EQ(stats.matches, 0u);
}

TEST(Lz77, ReplayReproducesRandomisedInputs) {
  std::mt19937 rng(2024);
  for (int trial = 0; trial < 40; ++trial) {
    const size_t n = rng() % 5000;
    std::vector<uint8_t> data(n);
    for (uint8_t& b : data)
      b = (rng() % 4) ? static_cast<uint8_t>('a' + rng() % 3) : static_cast<uint8_t>(rng() & 0xFF);
    for (int level : {1, 4, 9})
      CHECK_EQ(replay(lz77Parse(data.data(), data.size(), Lz77Params::forLevel(level))), data);
  }
}
