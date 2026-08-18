#include <random>
#include <string>

#include "autocomplete/kmp.hpp"
#include "test_framework.hpp"

using namespace autocomplete;

TEST(Kmp, FailureFunctionMatchesTheDefinition) {
  const Kmp kmp("ababaca");
  const std::vector<int> expected = {0, 0, 1, 2, 3, 0, 1};
  CHECK_EQ(kmp.failure(), expected);
}

TEST(Kmp, FailureFunctionOfARun) {
  const Kmp kmp("aaaa");
  const std::vector<int> expected = {0, 1, 2, 3};
  CHECK_EQ(kmp.failure(), expected);
}

TEST(Kmp, FindsAllOverlappingOccurrences) {
  const Kmp kmp("aa");
  const std::vector<size_t> hits = kmp.findAll("aaaa");
  const std::vector<size_t> expected = {0, 1, 2};
  CHECK_EQ(hits, expected);
}

TEST(Kmp, RespectsTheHitCap) {
  const Kmp kmp("ab");
  CHECK_EQ(kmp.findAll("ababab", 2).size(), 2u);
}

TEST(Kmp, HandlesMissingAndEdgeCases) {
  CHECK(!Kmp("xyz").contains("abcdef"));
  CHECK(Kmp("abc").contains("abc"));
  CHECK(Kmp("").findAll("abc").empty());       // empty pattern matches nothing
  CHECK(Kmp("abcd").findAll("abc").empty());   // pattern longer than the text
  CHECK_EQ(Kmp("c").find("abc"), 2u);
  CHECK_EQ(Kmp("q").find("abc"), std::string_view::npos);
}

TEST(Kmp, AgreesWithNaiveSearchOnRandomStrings) {
  std::mt19937 rng(31);
  for (int trial = 0; trial < 300; ++trial) {
    std::string text, pattern;
    for (size_t i = 0, n = rng() % 200; i < n; ++i) text += static_cast<char>('a' + rng() % 3);
    for (size_t i = 0, n = 1 + rng() % 5; i < n; ++i) pattern += static_cast<char>('a' + rng() % 3);

    std::vector<size_t> naive;
    if (pattern.size() <= text.size())
      for (size_t i = 0; i + pattern.size() <= text.size(); ++i)
        if (text.compare(i, pattern.size(), pattern) == 0) naive.push_back(i);

    CHECK_EQ(Kmp(pattern).findAll(text), naive);
  }
}

TEST(Kmp, ScanIsLinearOnTheAdversarialCase) {
  // The pattern that makes naive search quadratic: a^k b in a^n.
  const std::string text(20000, 'a');
  const Kmp kmp(std::string(500, 'a') + "b");
  CHECK(kmp.findAll(text).empty());
  // KMP never rescans text characters, so the work stays within a small
  // multiple of the text length (naive search would be ~500x this).
  CHECK(kmp.lastComparisons() < 3 * text.size());
}
