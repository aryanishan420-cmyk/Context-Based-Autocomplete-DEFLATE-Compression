#include "autocomplete/model.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

#include "autocomplete/tokenizer.hpp"

namespace autocomplete {
namespace {

inline uint64_t pairKey(uint32_t a, uint32_t b) {
  return (static_cast<uint64_t>(a) << 32) | b;
}

const std::vector<Successor>& emptySuccessors() {
  static const std::vector<Successor> kEmpty;
  return kEmpty;
}

// Little-endian fixed-width I/O so a model file moves between machines.
template <typename T>
void writeLe(FILE* f, T value) {
  uint8_t buf[sizeof(T)];
  for (size_t i = 0; i < sizeof(T); ++i)
    buf[i] = static_cast<uint8_t>((static_cast<uint64_t>(value) >> (8 * i)) & 0xFF);
  if (std::fwrite(buf, 1, sizeof(T), f) != sizeof(T))
    throw std::runtime_error("failed to write model file");
}

template <typename T>
T readLe(FILE* f) {
  uint8_t buf[sizeof(T)];
  if (std::fread(buf, 1, sizeof(T), f) != sizeof(T))
    throw std::runtime_error("truncated model file");
  uint64_t v = 0;
  for (size_t i = 0; i < sizeof(T); ++i) v |= static_cast<uint64_t>(buf[i]) << (8 * i);
  return static_cast<T>(v);
}

}  // namespace

uint32_t NgramModel::id(std::string_view word) const {
  auto it = index_.find(std::string(word));
  return it == index_.end() ? kNoWord : it->second;
}

uint64_t NgramModel::bigramCount(uint32_t prev, uint32_t w) const {
  const std::vector<Successor>& s = successors(prev);
  auto it = std::lower_bound(s.begin(), s.end(), w,
                             [](const Successor& x, uint32_t v) { return x.word < v; });
  return (it != s.end() && it->word == w) ? it->count : 0;
}

uint64_t NgramModel::trigramCount(uint32_t a, uint32_t b, uint32_t w) const {
  const std::vector<Successor>& s = successors2(a, b);
  auto it = std::lower_bound(s.begin(), s.end(), w,
                             [](const Successor& x, uint32_t v) { return x.word < v; });
  return (it != s.end() && it->word == w) ? it->count : 0;
}

uint64_t NgramModel::contextCount(uint32_t prev) const {
  uint64_t total = 0;
  for (const Successor& s : successors(prev)) total += s.count;
  return total;
}

uint64_t NgramModel::contextCount2(uint32_t a, uint32_t b) const {
  uint64_t total = 0;
  for (const Successor& s : successors2(a, b)) total += s.count;
  return total;
}

const std::vector<Successor>& NgramModel::successors(uint32_t prev) const {
  auto it = bigram_.find(prev);
  return it == bigram_.end() ? emptySuccessors() : it->second;
}

const std::vector<Successor>& NgramModel::successors2(uint32_t a, uint32_t b) const {
  auto it = trigram_.find(pairKey(a, b));
  return it == trigram_.end() ? emptySuccessors() : it->second;
}

uint32_t NgramModel::wordAtBlobOffset(size_t offset) const {
  // Offsets are ascending, so the word covering an offset is the last one
  // starting at or before it.
  auto it = std::upper_bound(blob_offsets_.begin(), blob_offsets_.end(), offset);
  if (it == blob_offsets_.begin()) return kNoWord;
  return static_cast<uint32_t>((it - blob_offsets_.begin()) - 1);
}

NgramModel NgramModel::build(std::string_view text, const BuildOptions& opts) {
  NgramModel m;
  const std::vector<std::string> tokens = tokenize(text);

  // Pass 1: word frequencies, so the vocabulary floor can be applied before
  // any n-gram is recorded.
  std::unordered_map<std::string, uint64_t> freq;
  freq.reserve(tokens.size() / 4 + 16);
  for (const std::string& t : tokens) freq[t]++;

  for (const auto& [w, c] : freq) {
    if (c < opts.min_word_count && w != kSentenceStart) continue;
    m.index_.emplace(w, static_cast<uint32_t>(m.words_.size()));
    m.words_.push_back(w);
    m.unigram_.push_back(c);
    m.total_tokens_ += c;
  }

  // Pass 2: n-grams.  A dropped word breaks the chain rather than joining its
  // neighbours together.
  std::unordered_map<uint64_t, uint32_t> bi;
  std::unordered_map<uint64_t, std::unordered_map<uint32_t, uint32_t>> tri;
  uint32_t prev2 = kNoWord, prev1 = kNoWord;
  for (const std::string& t : tokens) {
    const uint32_t cur = m.id(t);
    if (cur == kNoWord) {
      prev2 = prev1 = kNoWord;
      continue;
    }
    if (prev1 != kNoWord) {
      bi[pairKey(prev1, cur)]++;
      if (prev2 != kNoWord) tri[pairKey(prev2, prev1)][cur]++;
    }
    prev2 = prev1;
    prev1 = cur;
  }

  for (const auto& [key, count] : bi)
    m.bigram_[static_cast<uint32_t>(key >> 32)].push_back(
        {static_cast<uint32_t>(key & 0xFFFFFFFFu), count});
  for (auto& [ctx, succ] : tri) {
    std::vector<Successor> keep;
    for (const auto& [w, c] : succ)
      if (c >= opts.min_trigram_count) keep.push_back({w, c});
    if (!keep.empty()) m.trigram_.emplace(ctx, std::move(keep));
  }

  m.finishBuild(opts.trie_top_k);
  return m;
}

void NgramModel::finishBuild(size_t trie_top_k) {
  for (auto& [prev, succ] : bigram_)
    std::sort(succ.begin(), succ.end(),
              [](const Successor& a, const Successor& b) { return a.word < b.word; });
  for (auto& [ctx, succ] : trigram_)
    std::sort(succ.begin(), succ.end(),
              [](const Successor& a, const Successor& b) { return a.word < b.word; });

  blob_.clear();
  blob_offsets_.assign(words_.size(), 0);
  for (size_t i = 0; i < words_.size(); ++i) {
    blob_.push_back('\0');  // separator, so a search for "\0ab" anchors a prefix
    blob_offsets_[i] = blob_.size();
    blob_ += words_[i];
    if (words_[i] != kSentenceStart) trie_.insert(words_[i], static_cast<uint32_t>(i), unigram_[i]);
  }
  blob_.push_back('\0');
  trie_.finalize(trie_top_k);
}

ModelStats NgramModel::stats() const {
  ModelStats s;
  s.vocab = words_.size();
  s.tokens = total_tokens_;
  s.bigram_contexts = bigram_.size();
  for (const auto& [k, v] : bigram_) {
    (void)k;
    s.bigram_types += v.size();
  }
  s.trigram_contexts = trigram_.size();
  for (const auto& [k, v] : trigram_) {
    (void)k;
    s.trigram_types += v.size();
  }
  s.trie_nodes = trie_.nodeCount();
  s.trie_cached = trie_.cachedEntries();
  s.trie_bytes = trie_.approxMemoryBytes();
  return s;
}

void NgramModel::save(const std::string& path) const {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) throw std::runtime_error("cannot open model file for writing: " + path);
  struct Closer {
    FILE* f;
    ~Closer() { std::fclose(f); }
  } closer{f};

  std::fwrite("ACM1", 1, 4, f);
  writeLe<uint32_t>(f, 1);  // format version
  writeLe<uint64_t>(f, total_tokens_);
  writeLe<uint32_t>(f, static_cast<uint32_t>(words_.size()));
  for (size_t i = 0; i < words_.size(); ++i) {
    writeLe<uint16_t>(f, static_cast<uint16_t>(words_[i].size()));
    std::fwrite(words_[i].data(), 1, words_[i].size(), f);
    writeLe<uint64_t>(f, unigram_[i]);
  }
  writeLe<uint32_t>(f, static_cast<uint32_t>(bigram_.size()));
  for (const auto& [prev, succ] : bigram_) {
    writeLe<uint32_t>(f, prev);
    writeLe<uint32_t>(f, static_cast<uint32_t>(succ.size()));
    for (const Successor& s : succ) {
      writeLe<uint32_t>(f, s.word);
      writeLe<uint32_t>(f, s.count);
    }
  }
  writeLe<uint32_t>(f, static_cast<uint32_t>(trigram_.size()));
  for (const auto& [ctx, succ] : trigram_) {
    writeLe<uint64_t>(f, ctx);
    writeLe<uint32_t>(f, static_cast<uint32_t>(succ.size()));
    for (const Successor& s : succ) {
      writeLe<uint32_t>(f, s.word);
      writeLe<uint32_t>(f, s.count);
    }
  }
}

NgramModel NgramModel::load(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) throw std::runtime_error("cannot open model file: " + path);
  struct Closer {
    FILE* f;
    ~Closer() { std::fclose(f); }
  } closer{f};

  char magic[4];
  if (std::fread(magic, 1, 4, f) != 4 || std::string(magic, 4) != "ACM1")
    throw std::runtime_error("not an autocomplete model file: " + path);
  if (readLe<uint32_t>(f) != 1) throw std::runtime_error("unsupported model version");

  NgramModel m;
  m.total_tokens_ = readLe<uint64_t>(f);
  const uint32_t vocab = readLe<uint32_t>(f);
  m.words_.reserve(vocab);
  m.unigram_.reserve(vocab);
  for (uint32_t i = 0; i < vocab; ++i) {
    const uint16_t len = readLe<uint16_t>(f);
    std::string w(len, '\0');
    if (len && std::fread(w.data(), 1, len, f) != len)
      throw std::runtime_error("truncated model file");
    m.unigram_.push_back(readLe<uint64_t>(f));
    m.index_.emplace(w, i);
    m.words_.push_back(std::move(w));
  }
  const uint32_t bi_ctx = readLe<uint32_t>(f);
  for (uint32_t i = 0; i < bi_ctx; ++i) {
    const uint32_t prev = readLe<uint32_t>(f);
    const uint32_t n = readLe<uint32_t>(f);
    std::vector<Successor> succ(n);
    for (uint32_t k = 0; k < n; ++k) {
      succ[k].word = readLe<uint32_t>(f);
      succ[k].count = readLe<uint32_t>(f);
    }
    m.bigram_.emplace(prev, std::move(succ));
  }
  const uint32_t tri_ctx = readLe<uint32_t>(f);
  for (uint32_t i = 0; i < tri_ctx; ++i) {
    const uint64_t ctx = readLe<uint64_t>(f);
    const uint32_t n = readLe<uint32_t>(f);
    std::vector<Successor> succ(n);
    for (uint32_t k = 0; k < n; ++k) {
      succ[k].word = readLe<uint32_t>(f);
      succ[k].count = readLe<uint32_t>(f);
    }
    m.trigram_.emplace(ctx, std::move(succ));
  }

  m.finishBuild(32);
  return m;
}

}  // namespace autocomplete
