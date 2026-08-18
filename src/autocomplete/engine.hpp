// The suggestion engine.
//
// Candidates come from four places -- the trie's cached top-K for the prefix,
// the n-gram successors of the current context, KMP hits on the text typed so
// far in this session, and an LRU of recently accepted words -- and are then
// ranked by one scoring function that blends all of those signals.
#pragma once

#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "autocomplete/kmp.hpp"
#include "autocomplete/model.hpp"

namespace autocomplete {

struct Weights {
  double ngram = 1.0;      // weight on log P(word | context)
  double phrase = 0.85;    // repeated phrase in the current document
  double recency = 0.55;   // recently accepted by this user
  double prefix = 0.35;    // favour completions close to what was typed
  double substring = -1.5; // penalty for a mid-word (non-prefix) match

  // Jelinek-Mercer interpolation across n-gram orders.
  double lambda_trigram = 0.55;
  double lambda_bigram = 0.33;
  double lambda_unigram = 0.12;

  double alpha = 0.4;  // add-alpha smoothing for the unigram distribution
};

struct EngineConfig {
  Weights weights;
  size_t candidate_pool = 24;   // trie completions pulled per query
  size_t lru_capacity = 64;
  size_t max_phrase_hits = 256; // cap on KMP hits scanned per query
  size_t session_window = 16384;// bytes of recent text the KMP search covers
  size_t max_context_scan = 50000;
  bool use_context = true;      // off = unigram-only, for the eval ablation
};

// Where a candidate came from, for --explain and for the eval breakdown.
enum SourceFlag : uint32_t {
  kFromTrie = 1u << 0,
  kFromContext = 1u << 1,
  kFromPhrase = 1u << 2,
  kFromRecent = 1u << 3,
  kFromSubstring = 1u << 4,
};

struct Suggestion {
  std::string word;
  double score = 0.0;
  double p_unigram = 0.0;
  double p_bigram = 0.0;
  double p_trigram = 0.0;
  double p_interp = 0.0;
  double phrase_boost = 0.0;
  double recency_boost = 0.0;
  uint32_t sources = 0;
  std::string sourceNames() const;
};

class Engine {
 public:
  explicit Engine(const NgramModel& model, EngineConfig config = {});

  // Top-k completions of `prefix` given `context` (the words already typed).
  // An empty prefix turns this into next-word prediction.
  std::vector<Suggestion> suggest(std::string_view context, std::string_view prefix,
                                  size_t k) const;

  // Records a word the user chose: updates the LRU and the session text that
  // the KMP phrase search runs over.
  void accept(std::string_view word);

  // Feeds existing document text to the session index without touching the LRU.
  void observeText(std::string_view text);
  void clearSession();

  // Keeps the session buffer inside the configured window, trimming from the
  // front at a word boundary.
  void trimSession();

  const std::string& sessionText() const { return session_; }
  EngineConfig& config() { return config_; }
  const EngineConfig& config() const { return config_; }

 private:
  struct Candidate {
    uint32_t sources = 0;
    double phrase_hits = 0.0;
  };

  void addTrieCandidates(std::string_view prefix,
                         std::unordered_map<uint32_t, Candidate>& out) const;
  void addContextCandidates(const std::vector<uint32_t>& ctx, std::string_view prefix,
                            std::unordered_map<uint32_t, Candidate>& out) const;
  void addPhraseCandidates(const std::vector<std::string>& ctx_words,
                           std::string_view prefix,
                           std::unordered_map<uint32_t, Candidate>& out) const;
  void addRecentCandidates(std::string_view prefix,
                           std::unordered_map<uint32_t, Candidate>& out) const;
  void addSubstringCandidates(std::string_view needle, size_t wanted,
                              std::unordered_map<uint32_t, Candidate>& out) const;
  double recencyScore(uint32_t word) const;

  const NgramModel& model_;
  EngineConfig config_;

  // LRU of accepted words: a list for order, a map for O(1) lookup and move.
  mutable std::list<uint32_t> lru_;
  mutable std::unordered_map<uint32_t, std::list<uint32_t>::iterator> lru_index_;

  std::string session_;  // " word word word " form, searched with KMP
};

}  // namespace autocomplete
