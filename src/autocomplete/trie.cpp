#include "autocomplete/trie.hpp"

#include <algorithm>

namespace autocomplete {

int32_t Trie::childOf(int32_t node, char c) const {
  const auto& kids = nodes_[node].children;
  auto it = std::lower_bound(kids.begin(), kids.end(), c,
                             [](const std::pair<char, int32_t>& p, char v) {
                               return p.first < v;
                             });
  return (it != kids.end() && it->first == c) ? it->second : -1;
}

void Trie::insert(std::string_view word, uint32_t word_id, uint64_t count) {
  int32_t node = 0;
  for (char c : word) {
    auto& kids = nodes_[node].children;
    auto it = std::lower_bound(kids.begin(), kids.end(), c,
                               [](const std::pair<char, int32_t>& p, char v) {
                                 return p.first < v;
                               });
    if (it != kids.end() && it->first == c) {
      node = it->second;
      continue;
    }
    const int32_t child = static_cast<int32_t>(nodes_.size());
    // Reallocation invalidates `kids`, so take the insert position first.
    const size_t at = static_cast<size_t>(it - kids.begin());
    nodes_.emplace_back();
    nodes_[node].children.insert(nodes_[node].children.begin() + static_cast<long>(at),
                                 {c, child});
    node = child;
  }
  if (nodes_[node].word_id == kNoWord) ++words_;
  nodes_[node].word_id = word_id;
  nodes_[node].count = count;
}

void Trie::buildTop(int32_t node, size_t k) {
  Node& n = nodes_[node];
  std::vector<Completion> merged;

  if (n.word_id != kNoWord) merged.push_back({n.word_id, n.count});
  for (const auto& [c, child] : n.children) {
    buildTop(child, k);
    const std::vector<Completion>& sub = nodes_[child].top;
    merged.insert(merged.end(), sub.begin(), sub.end());
  }

  // Children are already sorted, so a partial sort is enough to keep the best k.
  const size_t keep = std::min(k, merged.size());
  std::partial_sort(merged.begin(), merged.begin() + static_cast<long>(keep),
                    merged.end(), [](const Completion& a, const Completion& b) {
                      if (a.count != b.count) return a.count > b.count;
                      return a.word_id < b.word_id;  // stable tie-break
                    });
  merged.resize(keep);
  merged.shrink_to_fit();
  n.top = std::move(merged);
}

void Trie::finalize(size_t k) {
  top_k_ = k;
  buildTop(0, k);
}

int32_t Trie::find(std::string_view prefix) const {
  int32_t node = 0;
  for (char c : prefix) {
    node = childOf(node, c);
    if (node < 0) return -1;
  }
  return node;
}

std::vector<Completion> Trie::topCompletions(std::string_view prefix, size_t k) const {
  const int32_t node = find(prefix);
  if (node < 0) return {};
  const std::vector<Completion>& top = nodes_[node].top;
  // Asking for more than the cache holds falls back to a full subtree walk so
  // the answer stays correct rather than silently truncated.
  if (k > top_k_ && top.size() >= top_k_) {
    std::vector<Completion> all = allCompletions(prefix, 0);
    const size_t keep = std::min(k, all.size());
    std::partial_sort(all.begin(), all.begin() + static_cast<long>(keep), all.end(),
                      [](const Completion& a, const Completion& b) {
                        if (a.count != b.count) return a.count > b.count;
                        return a.word_id < b.word_id;
                      });
    all.resize(keep);
    return all;
  }
  return std::vector<Completion>(top.begin(),
                                 top.begin() + static_cast<long>(std::min(k, top.size())));
}

void Trie::collect(int32_t node, std::vector<Completion>& out, size_t limit) const {
  if (limit && out.size() >= limit) return;
  const Node& n = nodes_[node];
  if (n.word_id != kNoWord) out.push_back({n.word_id, n.count});
  for (const auto& [c, child] : n.children) {
    (void)c;
    collect(child, out, limit);
    if (limit && out.size() >= limit) return;
  }
}

std::vector<Completion> Trie::allCompletions(std::string_view prefix, size_t limit) const {
  const int32_t node = find(prefix);
  std::vector<Completion> out;
  if (node >= 0) collect(node, out, limit);
  return out;
}

bool Trie::contains(std::string_view word) const {
  const int32_t node = find(word);
  return node >= 0 && nodes_[node].word_id != kNoWord;
}

size_t Trie::cachedEntries() const {
  size_t n = 0;
  for (const Node& node : nodes_) n += node.top.size();
  return n;
}

size_t Trie::approxMemoryBytes() const {
  size_t bytes = nodes_.size() * sizeof(Node);
  for (const Node& n : nodes_) {
    bytes += n.children.capacity() * sizeof(std::pair<char, int32_t>);
    bytes += n.top.capacity() * sizeof(Completion);
  }
  return bytes;
}

}  // namespace autocomplete
