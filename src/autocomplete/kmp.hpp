// Knuth-Morris-Pratt string matching.
//
// Used in two places by the autocomplete engine: scanning the vocabulary for
// mid-word matches when a prefix search comes up short, and scanning the
// session text for earlier occurrences of the phrase being typed, which is
// what lets suggestions adapt to the current document.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace autocomplete {

class Kmp {
 public:
  explicit Kmp(std::string pattern);

  // failure[i] = length of the longest proper prefix of pattern[0..i] that is
  // also a suffix of it.  This is what lets the scan never re-read a character.
  const std::vector<int>& failure() const { return failure_; }
  const std::string& pattern() const { return pattern_; }

  // Start offsets of every occurrence, up to `max_hits` (0 = unlimited).
  std::vector<size_t> findAll(std::string_view text, size_t max_hits = 0) const;

  // First occurrence, or std::string_view::npos.
  size_t find(std::string_view text) const;
  bool contains(std::string_view text) const { return find(text) != std::string_view::npos; }

  // Character comparisons performed by the last search on this object; kept so
  // the benchmarks can show the linear-time behaviour.
  size_t lastComparisons() const { return comparisons_; }

 private:
  std::string pattern_;
  std::vector<int> failure_;
  mutable size_t comparisons_ = 0;
};

}  // namespace autocomplete
