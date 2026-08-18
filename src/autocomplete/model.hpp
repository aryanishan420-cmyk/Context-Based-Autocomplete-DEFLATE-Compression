// Vocabulary + unigram/bigram/trigram counts gathered from a text corpus,
// with a binary on-disk form so the CLI starts instantly instead of re-reading
// the corpus every time.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "autocomplete/trie.hpp"

namespace autocomplete {

inline constexpr uint32_t kNoWord = UINT32_MAX;

struct Successor {
  uint32_t word;
  uint32_t count;
};

struct BuildOptions {
  uint32_t min_word_count = 1;      // drop rarer words from the vocabulary
  uint32_t min_trigram_count = 2;   // trigrams seen once are mostly noise
  size_t trie_top_k = 32;         // completions cached per trie node
};

struct ModelStats {
  size_t vocab = 0;
  uint64_t tokens = 0;
  size_t bigram_contexts = 0;
  uint64_t bigram_types = 0;
  size_t trigram_contexts = 0;
  uint64_t trigram_types = 0;
  size_t trie_nodes = 0;
  size_t trie_cached = 0;
  size_t trie_bytes = 0;
};

class NgramModel {
 public:
  // Counts n-grams over `text`.  Words below the frequency floor are dropped
  // from the vocabulary but still break n-gram continuity, so no false
  // adjacencies are invented.
  static NgramModel build(std::string_view text, const BuildOptions& opts = {});

  uint32_t id(std::string_view word) const;
  const std::string& word(uint32_t id) const { return words_[id]; }
  size_t vocabSize() const { return words_.size(); }
  uint64_t totalTokens() const { return total_tokens_; }

  uint64_t unigramCount(uint32_t w) const { return w < unigram_.size() ? unigram_[w] : 0; }
  uint64_t bigramCount(uint32_t prev, uint32_t w) const;
  uint64_t trigramCount(uint32_t a, uint32_t b, uint32_t w) const;
  uint64_t contextCount(uint32_t prev) const;
  uint64_t contextCount2(uint32_t a, uint32_t b) const;

  // Successors are sorted by word id: binary search for a count, linear scan
  // when generating candidates for a prefix.
  const std::vector<Successor>& successors(uint32_t prev) const;
  const std::vector<Successor>& successors2(uint32_t a, uint32_t b) const;

  const Trie& trie() const { return trie_; }

  // "\0word\0word..." over the whole vocabulary, for KMP substring search.
  const std::string& vocabBlob() const { return blob_; }
  uint32_t wordAtBlobOffset(size_t offset) const;

  ModelStats stats() const;

  void save(const std::string& path) const;
  static NgramModel load(const std::string& path);

 private:
  void finishBuild(size_t trie_top_k);

  std::vector<std::string> words_;
  std::unordered_map<std::string, uint32_t> index_;
  std::vector<uint64_t> unigram_;
  uint64_t total_tokens_ = 0;

  std::unordered_map<uint32_t, std::vector<Successor>> bigram_;
  std::unordered_map<uint64_t, std::vector<Successor>> trigram_;

  Trie trie_;
  std::string blob_;
  std::vector<size_t> blob_offsets_;  // parallel to words_, sorted ascending
};

}  // namespace autocomplete
