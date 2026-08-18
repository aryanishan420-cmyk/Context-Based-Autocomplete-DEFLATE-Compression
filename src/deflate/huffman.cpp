#include "deflate/huffman.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace deflate {
namespace {

// One coin in the package-merge "binary coin collector" problem: a weight plus
// the multiset of symbols that produced it, recorded as per-symbol counts.
// A symbol's final code length is how many times it appears in the solution.
struct Coin {
  uint64_t weight;
  std::vector<uint8_t> use;
};

}  // namespace

std::vector<uint8_t> packageMerge(const std::vector<uint64_t>& freq, int limit) {
  const size_t n = freq.size();
  std::vector<uint8_t> lengths(n, 0);

  // Only symbols that actually occur take part.
  std::vector<size_t> used;
  for (size_t i = 0; i < n; ++i)
    if (freq[i] > 0) used.push_back(i);

  if (used.empty()) return lengths;
  if (used.size() == 1) {
    // A one-symbol alphabet still needs a one-bit code to be decodable.
    lengths[used[0]] = 1;
    return lengths;
  }
  if (static_cast<uint64_t>(used.size()) > (1ull << limit))
    throw std::invalid_argument("alphabet too large for the code length limit");

  const size_t m = used.size();
  std::vector<Coin> base(m);
  for (size_t i = 0; i < m; ++i) {
    base[i].weight = freq[used[i]];
    base[i].use.assign(m, 0);
    base[i].use[i] = 1;
  }
  std::sort(base.begin(), base.end(),
            [](const Coin& a, const Coin& b) { return a.weight < b.weight; });

  // Level l holds coins of denomination 2^-l.  Start at the deepest level and
  // package pairs upward; each level mixes the packages from below with a
  // fresh copy of the original symbols.
  std::vector<Coin> level = base;
  for (int depth = 1; depth < limit; ++depth) {
    std::vector<Coin> packages;
    packages.reserve(level.size() / 2);
    for (size_t i = 0; i + 1 < level.size(); i += 2) {
      Coin merged;
      merged.weight = level[i].weight + level[i + 1].weight;
      merged.use = level[i].use;
      for (size_t s = 0; s < m; ++s) merged.use[s] += level[i + 1].use[s];
      packages.push_back(std::move(merged));
    }

    // Merge the sorted package list with the sorted symbol list.
    std::vector<Coin> next;
    next.reserve(packages.size() + m);
    size_t a = 0, b = 0;
    while (a < base.size() || b < packages.size()) {
      if (b == packages.size() ||
          (a < base.size() && base[a].weight <= packages[b].weight))
        next.push_back(base[a++]);
      else
        next.push_back(std::move(packages[b++]));
    }
    level = std::move(next);
  }

  // The cheapest 2m-2 coins of the top level form an optimal solution.
  const size_t take = 2 * m - 2;
  std::vector<uint32_t> counts(m, 0);
  for (size_t i = 0; i < take && i < level.size(); ++i)
    for (size_t s = 0; s < m; ++s) counts[s] += level[i].use[s];

  // `counts` is indexed by position in `used` (the marker bit that each base
  // coin carried through the merges), so the mapping back is direct.
  for (size_t i = 0; i < m; ++i) {
    if (counts[i] == 0 || counts[i] > static_cast<uint32_t>(limit))
      throw std::logic_error("package-merge produced an invalid code length");
    lengths[used[i]] = static_cast<uint8_t>(counts[i]);
  }

  return lengths;
}

HuffEncoder buildEncoder(const std::vector<uint8_t>& lengths) {
  HuffEncoder enc;
  enc.length = lengths;
  enc.code.assign(lengths.size(), 0);

  std::vector<uint32_t> count(kMaxCodeLen + 1, 0);
  for (uint8_t l : lengths) {
    if (l > kMaxCodeLen) throw std::invalid_argument("code length over 15 bits");
    if (l) count[l]++;
  }

  // Canonical assignment: shortest codes first, ascending within a length.
  std::vector<uint32_t> next(kMaxCodeLen + 2, 0);
  uint32_t code = 0;
  for (int bits = 1; bits <= kMaxCodeLen; ++bits) {
    code = (code + count[bits - 1]) << 1;
    next[bits] = code;
  }
  for (size_t sym = 0; sym < lengths.size(); ++sym)
    if (lengths[sym]) enc.code[sym] = next[lengths[sym]]++;

  return enc;
}

HuffDecoder buildDecoder(const std::vector<uint8_t>& lengths,
                         bool allow_incomplete) {
  HuffDecoder dec;
  dec.count.assign(kMaxCodeLen + 1, 0);
  for (uint8_t l : lengths) {
    if (l > kMaxCodeLen) throw CorruptStream("code length over 15 bits");
    dec.count[l]++;
  }
  dec.count[0] = 0;

  // Kraft inequality check: left == 0 means the code space is exactly filled.
  int left = 1;
  for (int len = 1; len <= kMaxCodeLen; ++len) {
    left <<= 1;
    left -= dec.count[len];
    if (left < 0) throw CorruptStream("over-subscribed Huffman code");
  }
  dec.complete = (left == 0);
  if (!dec.complete && !allow_incomplete)
    throw CorruptStream("incomplete Huffman code");

  std::vector<uint16_t> offset(kMaxCodeLen + 2, 0);
  for (int len = 1; len <= kMaxCodeLen; ++len)
    offset[len + 1] = offset[len] + dec.count[len];

  dec.symbol.assign(lengths.size(), 0);
  for (size_t sym = 0; sym < lengths.size(); ++sym)
    if (lengths[sym]) dec.symbol[offset[lengths[sym]]++] = sym;

  return dec;
}

int decodeSymbol(BitReader& in, const HuffDecoder& dec) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= kMaxCodeLen; ++len) {
    code |= in.getBit();
    int count = dec.count[len];
    if (code - first < count) return dec.symbol[index + (code - first)];
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  throw CorruptStream("invalid Huffman code");
}

uint64_t encodedBits(const std::vector<uint64_t>& freq,
                     const std::vector<uint8_t>& lengths) {
  uint64_t bits = 0;
  for (size_t i = 0; i < freq.size(); ++i) bits += freq[i] * lengths[i];
  return bits;
}

}  // namespace deflate
