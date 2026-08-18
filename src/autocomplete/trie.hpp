// Prefix trie with cached top-K completions.
//
// A plain trie answers "which words start with this prefix?" but has to walk
// the whole subtree to rank them.  This one precomputes, for every node, the K
// highest-frequency words underneath it, so a prefix query costs O(|prefix|)
// lookups plus a copy of K entries -- independent of how many words share the
// prefix.  That cache is the extension the autocomplete engine is built on.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace autocomplete {

struct Completion {
  uint32_t word_id;
  uint64_t count;
};

class Trie {
 public:
  Trie() : nodes_(1) {}

  void insert(std::string_view word, uint32_t word_id, uint64_t count);

  // Precomputes each node's top-K list.  Must be called after the last insert.
  void finalize(size_t k);

  // Highest-frequency completions of `prefix`, best first.  Empty if the
  // prefix is not in the trie.  Requires finalize() to have been called.
  std::vector<Completion> topCompletions(std::string_view prefix, size_t k) const;

  // Every word under `prefix` (no ranking), capped at `limit` (0 = unlimited).
  std::vector<Completion> allCompletions(std::string_view prefix, size_t limit = 0) const;

  bool contains(std::string_view word) const;
  bool hasPrefix(std::string_view prefix) const { return find(prefix) >= 0; }

  size_t wordCount() const { return words_; }
  size_t nodeCount() const { return nodes_.size(); }
  size_t cachedEntries() const;
  size_t approxMemoryBytes() const;

 private:
  struct Node {
    // Children kept sorted by character: binary search on lookup, and a small
    // vector beats a 26-slot array once most nodes have one or two children.
    std::vector<std::pair<char, int32_t>> children;
    std::vector<Completion> top;  // filled by finalize()
    uint32_t word_id = kNoWord;
    uint64_t count = 0;
  };
  static constexpr uint32_t kNoWord = UINT32_MAX;

  int32_t find(std::string_view prefix) const;
  int32_t childOf(int32_t node, char c) const;
  void buildTop(int32_t node, size_t k);
  void collect(int32_t node, std::vector<Completion>& out, size_t limit) const;

  std::vector<Node> nodes_;
  size_t words_ = 0;
  size_t top_k_ = 0;
};

}  // namespace autocomplete
