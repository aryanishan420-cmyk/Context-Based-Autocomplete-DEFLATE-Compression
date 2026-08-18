#include <algorithm>
#include <random>
#include <string>

#include "autocomplete/trie.hpp"
#include "test_framework.hpp"

using namespace autocomplete;

namespace {

Trie buildTrie(const std::vector<std::pair<std::string, uint64_t>>& words, size_t k = 8) {
  Trie trie;
  uint32_t id = 0;
  for (const auto& [word, count] : words) trie.insert(word, id++, count);
  trie.finalize(k);
  return trie;
}

}  // namespace

TEST(Trie, StoresAndFindsWords) {
  const Trie trie = buildTrie({{"cat", 5}, {"car", 9}, {"cart", 1}, {"dog", 4}});
  CHECK(trie.contains("cat"));
  CHECK(trie.contains("cart"));
  CHECK(!trie.contains("ca"));      // a prefix is not a word
  CHECK(!trie.contains("cats"));
  CHECK(trie.hasPrefix("ca"));
  CHECK(!trie.hasPrefix("z"));
  CHECK_EQ(trie.wordCount(), 4u);
}

TEST(Trie, RanksCompletionsByFrequency) {
  const Trie trie = buildTrie({{"car", 9}, {"cat", 5}, {"cart", 1}, {"cab", 7}});
  const std::vector<Completion> top = trie.topCompletions("ca", 3);
  CHECK_EQ(top.size(), 3u);
  CHECK_EQ(top[0].count, 9u);  // car
  CHECK_EQ(top[1].count, 7u);  // cab
  CHECK_EQ(top[2].count, 5u);  // cat
}

TEST(Trie, ExactWordIsItsOwnCompletion) {
  const Trie trie = buildTrie({{"the", 100}, {"there", 30}, {"they", 60}});
  const std::vector<Completion> top = trie.topCompletions("the", 3);
  CHECK_EQ(top.size(), 3u);
  CHECK_EQ(top[0].count, 100u);
}

TEST(Trie, UnknownPrefixYieldsNothing) {
  const Trie trie = buildTrie({{"alpha", 3}, {"beta", 2}});
  CHECK(trie.topCompletions("gamma", 5).empty());
  CHECK(trie.allCompletions("zz").empty());
}

TEST(Trie, AskingForMoreThanTheCacheFallsBackToAFullWalk) {
  // The cache holds 2 entries per node, but a query for 5 must still be right.
  std::vector<std::pair<std::string, uint64_t>> words;
  for (int i = 0; i < 20; ++i)
    words.push_back({"pre" + std::string(1, static_cast<char>('a' + i)), static_cast<uint64_t>(i + 1)});
  const Trie trie = buildTrie(words, /*k=*/2);

  const std::vector<Completion> top = trie.topCompletions("pre", 5);
  CHECK_EQ(top.size(), 5u);
  for (size_t i = 0; i + 1 < top.size(); ++i) CHECK(top[i].count >= top[i + 1].count);
  CHECK_EQ(top[0].count, 20u);
  CHECK_EQ(top[4].count, 16u);
}

TEST(Trie, CachedRankingAgreesWithABruteForceScan) {
  std::mt19937 rng(7);
  std::vector<std::pair<std::string, uint64_t>> words;
  for (int i = 0; i < 500; ++i) {
    std::string w;
    for (size_t n = 1 + rng() % 6; w.size() < n;) w += static_cast<char>('a' + rng() % 4);
    words.push_back({w, 1 + rng() % 1000});
  }
  // Deduplicate: the trie keeps one entry per distinct word.
  std::sort(words.begin(), words.end());
  words.erase(std::unique(words.begin(), words.end(),
                          [](const auto& a, const auto& b) { return a.first == b.first; }),
              words.end());
  const Trie trie = buildTrie(words, 8);

  for (const char* prefix : {"a", "ab", "abc", "b", "cd"}) {
    std::vector<uint64_t> brute;
    for (const auto& [w, c] : words)
      if (w.rfind(prefix, 0) == 0) brute.push_back(c);
    std::sort(brute.rbegin(), brute.rend());
    brute.resize(std::min<size_t>(brute.size(), 5));

    std::vector<uint64_t> got;
    for (const Completion& c : trie.topCompletions(prefix, 5)) got.push_back(c.count);
    CHECK_EQ(got, brute);
  }
}

TEST(Trie, AllCompletionsRespectsItsLimit) {
  const Trie trie = buildTrie({{"aa", 1}, {"ab", 2}, {"ac", 3}, {"ba", 4}});
  CHECK_EQ(trie.allCompletions("a").size(), 3u);
  CHECK_EQ(trie.allCompletions("a", 2).size(), 2u);
  CHECK_EQ(trie.allCompletions("").size(), 4u);
}
