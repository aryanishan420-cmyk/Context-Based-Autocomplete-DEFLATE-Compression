#include <algorithm>
#include <cstdio>
#include <string>

#include "autocomplete/engine.hpp"
#include "autocomplete/eval.hpp"
#include "autocomplete/model.hpp"
#include "autocomplete/tokenizer.hpp"
#include "test_framework.hpp"

using namespace autocomplete;

namespace {

// A small corpus with deliberate, checkable regularities.
const char* kCorpus = R"(
The cat sat on the mat. The cat drank the milk.
The dog sat on the log. The dog chased the cat.
Machine learning models learn patterns. Machine learning needs data.
Compression algorithms compress data. Compression algorithms need entropy.
The quick brown fox jumps over the lazy dog. The quick brown fox is quick.
)";

std::vector<std::string> words(const std::vector<Suggestion>& suggestions) {
  std::vector<std::string> out;
  for (const Suggestion& s : suggestions) out.push_back(s.word);
  return out;
}

bool contains(const std::vector<std::string>& list, const std::string& value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

NgramModel smallModel() {
  BuildOptions opts;
  opts.min_trigram_count = 1;
  return NgramModel::build(kCorpus, opts);
}

}  // namespace

TEST(Tokenizer, SplitsWordsAndLowercases) {
  const std::vector<std::string> t = tokenize("Hello, World!  It's fine.", false);
  const std::vector<std::string> expected = {"hello", "world", "it's", "fine"};
  CHECK_EQ(t, expected);
}

TEST(Tokenizer, MarksSentenceBoundaries) {
  const std::vector<std::string> t = tokenize("One two. Three!");
  const std::vector<std::string> expected = {"<s>", "one", "two", "<s>", "three", "<s>"};
  CHECK_EQ(t, expected);
}

TEST(Tokenizer, NormalizeStripsPunctuation) {
  CHECK_EQ(normalize("Hello!"), std::string("hello"));
  CHECK_EQ(normalize("don't"), std::string("don't"));
  CHECK_EQ(normalize("...."), std::string(""));
}

TEST(Tokenizer, ContextWordsKeepsTheTail) {
  const std::vector<std::string> t = contextWords("a b c d e", 2);
  const std::vector<std::string> expected = {"d", "e"};
  CHECK_EQ(t, expected);
}

TEST(Model, CountsUnigramsAndBigrams) {
  const NgramModel model = smallModel();
  const uint32_t the = model.id("the");
  const uint32_t cat = model.id("cat");
  CHECK(the != kNoWord);
  CHECK(cat != kNoWord);
  CHECK_EQ(model.unigramCount(cat), 3u);       // three occurrences of "cat"
  CHECK_EQ(model.bigramCount(the, cat), 3u);   // always preceded by "the"
  CHECK_EQ(model.bigramCount(cat, the), 0u);
  CHECK_EQ(model.id("notinthecorpus"), kNoWord);
}

TEST(Model, CountsTrigrams) {
  const NgramModel model = smallModel();
  const uint32_t machine = model.id("machine");
  const uint32_t learning = model.id("learning");
  const uint32_t models = model.id("models");
  CHECK_EQ(model.trigramCount(machine, learning, models), 1u);
  CHECK_EQ(model.contextCount2(machine, learning), 2u);  // "models" and "needs"
}

TEST(Model, MinWordCountPrunesTheVocabulary) {
  BuildOptions opts;
  opts.min_word_count = 3;
  const NgramModel model = NgramModel::build(kCorpus, opts);
  CHECK(model.id("the") != kNoWord);      // very frequent
  CHECK_EQ(model.id("milk"), kNoWord);    // appears once
}

TEST(Model, SurvivesSaveAndLoad) {
  const NgramModel model = smallModel();
  const std::string path = "/tmp/autocomplete_model_test.bin";
  model.save(path);
  const NgramModel loaded = NgramModel::load(path);

  CHECK_EQ(loaded.vocabSize(), model.vocabSize());
  CHECK_EQ(loaded.totalTokens(), model.totalTokens());
  const uint32_t the = model.id("the");
  const uint32_t cat = model.id("cat");
  CHECK_EQ(loaded.unigramCount(loaded.id("cat")), model.unigramCount(cat));
  CHECK_EQ(loaded.bigramCount(loaded.id("the"), loaded.id("cat")),
           model.bigramCount(the, cat));
  CHECK(loaded.trie().contains("compression"));
  std::remove(path.c_str());
}

TEST(Model, RejectsAFileThatIsNotAModel) {
  const std::string path = "/tmp/autocomplete_not_a_model.bin";
  FILE* f = std::fopen(path.c_str(), "wb");
  std::fputs("this is not a model", f);
  std::fclose(f);
  CHECK_THROWS(NgramModel::load(path));
  std::remove(path.c_str());
}

TEST(Engine, CompletesFromThePrefixAlone) {
  const NgramModel model = smallModel();
  const Engine engine(model);
  CHECK(contains(words(engine.suggest("", "comp", 5)), "compression"));
  CHECK(contains(words(engine.suggest("", "mach", 5)), "machine"));
}

TEST(Engine, ContextChangesTheRanking) {
  const NgramModel model = smallModel();
  const Engine engine(model);

  // "the" is followed by cat/dog/mat/log/milk/quick/lazy; after "machine
  // learning" the only continuations are "models" and "needs".
  const std::vector<std::string> with_context = words(engine.suggest("machine learning", "", 3));
  CHECK(contains(with_context, "models") || contains(with_context, "needs"));

  EngineConfig no_ctx;
  no_ctx.use_context = false;
  const Engine plain(model, no_ctx);
  const std::vector<std::string> without = words(plain.suggest("machine learning", "", 3));
  CHECK(!without.empty());
  CHECK(with_context != without);
}

TEST(Engine, ContextPicksBetweenWordsSharingAPrefix) {
  // "cat" and "chased" both start with "c"; the context should decide.
  const NgramModel model = smallModel();
  const Engine engine(model);
  const std::vector<std::string> after_the = words(engine.suggest("the", "c", 3));
  CHECK_EQ(after_the.front(), std::string("cat"));
  const std::vector<std::string> after_dog = words(engine.suggest("the dog", "c", 3));
  CHECK_EQ(after_dog.front(), std::string("chased"));
}

TEST(Engine, EmptyPrefixPredictsTheNextWord) {
  const NgramModel model = smallModel();
  const Engine engine(model);
  const std::vector<Suggestion> s = engine.suggest("the quick brown", "", 3);
  CHECK(!s.empty());
  CHECK_EQ(s.front().word, std::string("fox"));
}

TEST(Engine, PhraseSearchLearnsFromTheCurrentSession) {
  const NgramModel model = smallModel();
  Engine engine(model);

  // "data" follows "compression algorithms" nowhere in the corpus with a "d"
  // prefix advantage, but repeating a phrase in this session should promote
  // whatever followed it here.
  for (const std::string& w : tokenize("machine learning needs data", false)) engine.accept(w);
  for (const std::string& w : tokenize("machine learning needs data", false)) engine.accept(w);

  const std::vector<Suggestion> s = engine.suggest("machine learning needs", "d", 3);
  CHECK(!s.empty());
  CHECK_EQ(s.front().word, std::string("data"));
  CHECK(s.front().phrase_boost > 0.0);
}

TEST(Engine, RecentlyAcceptedWordsAreFavoured) {
  const NgramModel model = smallModel();
  Engine engine(model);
  const std::vector<std::string> before = words(engine.suggest("", "l", 5));
  CHECK(before.front() != std::string("log"));  // "learning" is more frequent

  engine.accept("log");
  const std::vector<Suggestion> after = engine.suggest("", "l", 5);
  CHECK_EQ(after.front().word, std::string("log"));
  CHECK(after.front().recency_boost > 0.0);
  CHECK(before != words(after));
}

TEST(Engine, KmpSubstringFallbackFindsMidWordMatches) {
  const NgramModel model = smallModel();
  const Engine engine(model);
  // No word starts with "ression", but "compression" contains it.
  const std::vector<Suggestion> s = engine.suggest("", "ression", 3);
  CHECK(!s.empty());
  CHECK_EQ(s.front().word, std::string("compression"));
  CHECK((s.front().sources & kFromSubstring) != 0);
}

TEST(Engine, UnknownPrefixReturnsNothing) {
  const NgramModel model = smallModel();
  const Engine engine(model);
  CHECK(engine.suggest("", "zzzzq", 5).empty());
}

TEST(Engine, SessionWindowStaysBounded) {
  const NgramModel model = smallModel();
  EngineConfig cfg;
  cfg.session_window = 256;
  Engine engine(model, cfg);
  for (int i = 0; i < 2000; ++i) engine.accept("compression");
  CHECK(engine.sessionText().size() <= 256 + 32);
}

TEST(Engine, SuggestionsAreRankedByScore) {
  const NgramModel model = smallModel();
  const Engine engine(model);
  const std::vector<Suggestion> s = engine.suggest("the", "", 8);
  for (size_t i = 0; i + 1 < s.size(); ++i) CHECK(s[i].score >= s[i + 1].score);
}

TEST(Eval, ReportsSaneMetricsAndContextHelps) {
  const NgramModel model = smallModel();

  EvalOptions opts;
  opts.prefix_len = 1;
  opts.k = 5;
  opts.use_context = true;
  const EvalResult with_ctx = evaluate(model, kCorpus, opts);

  opts.use_context = false;
  const EvalResult without = evaluate(model, kCorpus, opts);

  CHECK(with_ctx.queries > 0);
  CHECK(with_ctx.top1 <= with_ctx.top3 + 1e-9);
  CHECK(with_ctx.top3 <= with_ctx.topk + 1e-9);
  CHECK(with_ctx.mrr >= 0.0 && with_ctx.mrr <= 1.0);
  CHECK(with_ctx.top1 > without.top1);  // context must beat frequency alone
}
