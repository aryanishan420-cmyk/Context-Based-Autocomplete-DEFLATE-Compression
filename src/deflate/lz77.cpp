#include "deflate/lz77.hpp"

#include <algorithm>
#include <cstring>

namespace deflate {
namespace {

constexpr int kHashBits = 15;
constexpr size_t kHashSize = 1u << kHashBits;
constexpr uint32_t kHashMask = kHashSize - 1;

inline uint32_t hash3(const uint8_t* p) {
  return ((static_cast<uint32_t>(p[0]) << 10) ^
          (static_cast<uint32_t>(p[1]) << 5) ^ static_cast<uint32_t>(p[2])) &
         kHashMask;
}

}  // namespace

Lz77Params Lz77Params::forLevel(int level) {
  // level 1 is fastest, 9 searches hardest.  Level 0 never matches (stored).
  static const Lz77Params kTable[10] = {
      {0, 0, 0, false},          // 0: no matching at all (stored/Huffman only)
      {4, 8, 0, false},          // 1: greedy, shortest chains
      {8, 16, 0, false},         // 2
      {32, 32, 0, false},        // 3: greedy, longer chains
      {32, 32, 16, true},        // 4: lazy matching starts here
      {64, 48, 32, true},        // 5
      {128, 128, 64, true},      // 6 (default)
      {256, 192, 128, true},     // 7
      {1024, 258, 258, true},    // 8
      {4096, 258, 258, true},    // 9: exhaustive
  };
  return kTable[std::min(std::max(level, 0), 9)];
}

std::vector<Token> lz77Parse(const uint8_t* data, size_t size,
                             const Lz77Params& params, Lz77Stats* stats) {
  std::vector<Token> tokens;
  if (size == 0) return tokens;
  tokens.reserve(size / 2 + 8);

  Lz77Stats local;
  Lz77Stats& st = stats ? *stats : local;

  if (params.max_chain == 0 || size < static_cast<size_t>(kMinMatch)) {
    for (size_t i = 0; i < size; ++i) tokens.push_back(Token::literal(data[i]));
    st.literals += size;
    return tokens;
  }

  // head[h] is the most recent position with hash h; prev[p] chains backwards.
  std::vector<int32_t> head(kHashSize, -1);
  std::vector<int32_t> prev(size, -1);

  auto insert = [&](size_t pos) {
    if (pos + kMinMatch > size) return;
    uint32_t h = hash3(data + pos);
    prev[pos] = head[h];
    head[h] = static_cast<int32_t>(pos);
  };

  // Longest match for `pos`, searching only positions inside the 32 KiB window.
  // Returns the length (0 if none) and writes the distance to `out_dist`.
  auto findMatch = [&](size_t pos, int min_len, uint16_t* out_dist) -> int {
    if (pos + kMinMatch > size) return 0;
    const size_t limit = std::min<size_t>(kMaxMatch, size - pos);
    if (limit < static_cast<size_t>(kMinMatch)) return 0;

    const size_t window_start = pos > kWindowSize ? pos - kWindowSize : 0;
    int best_len = std::max(min_len, kMinMatch - 1);
    uint16_t best_dist = 0;

    // `pos` has already been inserted at the head of its chain, so start one
    // link back -- otherwise the position matches itself at distance 0.
    int32_t cand = prev[pos];
    int chain = params.max_chain;
    while (cand >= 0 && chain-- > 0) {
      const size_t cpos = static_cast<size_t>(cand);
      if (cpos < window_start) break;  // chain is ordered, so we are done
      st.chain_steps++;

      // Cheap rejection: the byte just past the current best must match.
      if (data[cpos + best_len] == data[pos + best_len] &&
          data[cpos] == data[pos]) {
        size_t len = 0;
        while (len < limit && data[cpos + len] == data[pos + len]) ++len;
        if (static_cast<int>(len) > best_len) {
          best_len = static_cast<int>(len);
          best_dist = static_cast<uint16_t>(pos - cpos);
          if (best_len >= params.nice_length || len == limit) break;
        }
      }
      cand = prev[cpos];
    }

    if (best_len < kMinMatch || best_dist == 0) return 0;
    *out_dist = best_dist;
    return best_len;
  };

  size_t pos = 0;
  int pending_len = 0;             // lazy match held back from the previous step
  uint16_t pending_dist = 0;
  size_t pending_pos = 0;

  while (pos < size) {
    insert(pos);

    uint16_t dist = 0;
    int len = findMatch(pos, 0, &dist);

    if (params.lazy) {
      if (pending_len >= kMinMatch) {
        if (len > pending_len) {
          // The new match is better: emit the held byte as a literal and keep
          // looking from here.
          tokens.push_back(Token::literal(data[pending_pos]));
          st.literals++;
          pending_len = (len >= params.max_lazy) ? 0 : len;
          pending_dist = dist;
          pending_pos = pos;
          if (pending_len == 0) {  // long enough: take it now
            tokens.push_back(Token::match(static_cast<uint16_t>(len), dist));
            st.matches++;
            st.matched_bytes += len;
            for (size_t k = 1; k < static_cast<size_t>(len); ++k) insert(pos + k);
            pos += len;
            continue;
          }
          ++pos;
          continue;
        }
        // The held match still wins: emit it.
        tokens.push_back(Token::match(static_cast<uint16_t>(pending_len), pending_dist));
        st.matches++;
        st.matched_bytes += pending_len;
        const size_t end = pending_pos + pending_len;
        for (size_t k = pos + 1; k < end; ++k) insert(k);
        pos = end;
        pending_len = 0;
        continue;
      }

      if (len >= kMinMatch && len < params.max_lazy && pos + 1 < size) {
        // Hold this match for one byte to see if the next position beats it.
        pending_len = len;
        pending_dist = dist;
        pending_pos = pos;
        ++pos;
        continue;
      }
    }

    if (len >= kMinMatch) {
      tokens.push_back(Token::match(static_cast<uint16_t>(len), dist));
      st.matches++;
      st.matched_bytes += len;
      for (size_t k = 1; k < static_cast<size_t>(len); ++k) insert(pos + k);
      pos += len;
    } else {
      tokens.push_back(Token::literal(data[pos]));
      st.literals++;
      ++pos;
    }
  }

  // A lazy match may still be outstanding when the input ends.
  if (pending_len >= kMinMatch) {
    tokens.push_back(Token::match(static_cast<uint16_t>(pending_len), pending_dist));
    st.matches++;
    st.matched_bytes += pending_len;
  }

  return tokens;
}

}  // namespace deflate
