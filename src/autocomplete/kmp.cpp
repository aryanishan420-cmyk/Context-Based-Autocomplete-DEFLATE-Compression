#include "autocomplete/kmp.hpp"

namespace autocomplete {

Kmp::Kmp(std::string pattern) : pattern_(std::move(pattern)) {
  failure_.assign(pattern_.size(), 0);
  // Build the failure function by matching the pattern against itself.
  for (size_t i = 1; i < pattern_.size(); ++i) {
    int len = failure_[i - 1];
    while (len > 0 && pattern_[i] != pattern_[len]) len = failure_[len - 1];
    if (pattern_[i] == pattern_[len]) ++len;
    failure_[i] = len;
  }
}

std::vector<size_t> Kmp::findAll(std::string_view text, size_t max_hits) const {
  std::vector<size_t> hits;
  comparisons_ = 0;
  const size_t m = pattern_.size();
  if (m == 0 || text.size() < m) return hits;

  size_t matched = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    // On a mismatch, slide the pattern by consulting the failure function
    // instead of backing up in the text.
    while (matched > 0 && text[i] != pattern_[matched]) {
      ++comparisons_;
      matched = static_cast<size_t>(failure_[matched - 1]);
    }
    ++comparisons_;
    if (text[i] == pattern_[matched]) ++matched;
    if (matched == m) {
      hits.push_back(i + 1 - m);
      if (max_hits && hits.size() >= max_hits) return hits;
      matched = static_cast<size_t>(failure_[m - 1]);
    }
  }
  return hits;
}

size_t Kmp::find(std::string_view text) const {
  const std::vector<size_t> hits = findAll(text, 1);
  return hits.empty() ? std::string_view::npos : hits[0];
}

}  // namespace autocomplete
