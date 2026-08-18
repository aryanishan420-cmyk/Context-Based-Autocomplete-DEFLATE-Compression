#include "autocomplete/engine.hpp"

#include <algorithm>
#include <cmath>

#include "autocomplete/tokenizer.hpp"

namespace autocomplete {
namespace {

bool startsWith(const std::string& word, std::string_view prefix) {
  return word.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), word.begin());
}

constexpr size_t kMaxPhraseOrder = 3;
// A single-word "phrase" would only repeat what the bigram model already
// captures, so the document search starts at two words -- that is where it
// adds information the counts do not have.
constexpr size_t kMinPhraseOrder = 2;

}  // namespace

std::string Suggestion::sourceNames() const {
  std::string s;
  auto add = [&](uint32_t flag, const char* name) {
    if (sources & flag) {
      if (!s.empty()) s += "+";
      s += name;
    }
  };
  add(kFromTrie, "trie");
  add(kFromContext, "ngram");
  add(kFromPhrase, "phrase");
  add(kFromRecent, "recent");
  add(kFromSubstring, "substr");
  return s.empty() ? "none" : s;
}

Engine::Engine(const NgramModel& model, EngineConfig config)
    : model_(model), config_(std::move(config)) {
  session_ = " ";
}

void Engine::addTrieCandidates(std::string_view prefix,
                               std::unordered_map<uint32_t, Candidate>& out) const {
  // An empty prefix lands on the root node, whose cached list is simply the
  // most frequent words overall -- the right fallback when there is nothing
  // else to go on.
  for (const Completion& c : model_.trie().topCompletions(prefix, config_.candidate_pool))
    out[c.word_id].sources |= kFromTrie;
}

void Engine::addContextCandidates(const std::vector<uint32_t>& ctx, std::string_view prefix,
                                  std::unordered_map<uint32_t, Candidate>& out) const {
  if (!config_.use_context || ctx.empty()) return;

  auto take = [&](const std::vector<Successor>& succ) {
    size_t scanned = 0;
    for (const Successor& s : succ) {
      if (++scanned > config_.max_context_scan) break;
      if (prefix.empty() || startsWith(model_.word(s.word), prefix))
        out[s.word].sources |= kFromContext;
    }
  };

  const uint32_t last = ctx.back();
  if (last != kNoWord) take(model_.successors(last));
  if (ctx.size() >= 2 && ctx[ctx.size() - 2] != kNoWord && last != kNoWord)
    take(model_.successors2(ctx[ctx.size() - 2], last));
}

void Engine::addPhraseCandidates(const std::vector<std::string>& ctx_words,
                                 std::string_view prefix,
                                 std::unordered_map<uint32_t, Candidate>& out) const {
  if (!config_.use_context || ctx_words.empty() || session_.size() <= 1) return;

  // Try the longest phrase first; a 3-word repeat is far stronger evidence
  // than a 1-word one, so stop as soon as a length produces hits.
  const size_t max_order = std::min(kMaxPhraseOrder, ctx_words.size());
  if (max_order < kMinPhraseOrder) return;
  for (size_t order = max_order; order >= kMinPhraseOrder; --order) {
    std::string pattern = " ";
    for (size_t i = ctx_words.size() - order; i < ctx_words.size(); ++i)
      pattern += ctx_words[i] + " ";

    const Kmp kmp(pattern);
    const std::vector<size_t> hits = kmp.findAll(session_, config_.max_phrase_hits);
    if (hits.empty()) continue;

    bool any = false;
    for (size_t hit : hits) {
      const size_t start = hit + pattern.size();
      if (start >= session_.size()) continue;
      const size_t end = session_.find(' ', start);
      if (end == std::string::npos) continue;
      const std::string next = session_.substr(start, end - start);
      if (next.empty() || !startsWith(next, prefix)) continue;
      const uint32_t id = model_.id(next);
      if (id == kNoWord) continue;
      Candidate& c = out[id];
      c.sources |= kFromPhrase;
      c.phrase_hits += static_cast<double>(order);  // weight by phrase length
      any = true;
    }
    if (any) return;
  }
}

void Engine::addRecentCandidates(std::string_view prefix,
                                 std::unordered_map<uint32_t, Candidate>& out) const {
  for (uint32_t id : lru_)
    if (prefix.empty() || startsWith(model_.word(id), prefix))
      out[id].sources |= kFromRecent;
}

void Engine::addSubstringCandidates(std::string_view needle, size_t wanted,
                                    std::unordered_map<uint32_t, Candidate>& out) const {
  if (needle.empty() || out.size() >= wanted) return;
  // One KMP pass over the whole vocabulary blob finds every word containing the
  // query, in O(|vocab| + |query|) -- the fallback for typos and mid-word edits.
  const Kmp kmp{std::string(needle)};
  for (size_t hit : kmp.findAll(model_.vocabBlob(), wanted * 8)) {
    const uint32_t id = model_.wordAtBlobOffset(hit);
    if (id == kNoWord) continue;
    out[id].sources |= kFromSubstring;
  }
}

double Engine::recencyScore(uint32_t word) const {
  size_t rank = 0;
  for (uint32_t id : lru_) {
    if (id == word) return 1.0 / (1.0 + static_cast<double>(rank));
    ++rank;
  }
  return 0.0;
}

std::vector<Suggestion> Engine::suggest(std::string_view context, std::string_view prefix,
                                        size_t k) const {
  const std::string norm_prefix = normalize(prefix);
  const std::vector<std::string> ctx_words = contextWords(context, kMaxPhraseOrder);
  std::vector<uint32_t> ctx_ids;
  ctx_ids.reserve(ctx_words.size());
  for (const std::string& w : ctx_words) ctx_ids.push_back(model_.id(w));

  std::unordered_map<uint32_t, Candidate> candidates;
  addTrieCandidates(norm_prefix, candidates);
  addContextCandidates(ctx_ids, norm_prefix, candidates);
  addPhraseCandidates(ctx_words, norm_prefix, candidates);
  addRecentCandidates(norm_prefix, candidates);
  if (candidates.size() < k) addSubstringCandidates(norm_prefix, k, candidates);

  const Weights& w = config_.weights;
  const double vocab = static_cast<double>(model_.vocabSize());
  const double total = static_cast<double>(model_.totalTokens());

  // Context counts are computed once per query, not per candidate.
  const uint32_t last = ctx_ids.empty() ? kNoWord : ctx_ids.back();
  const uint32_t prev = ctx_ids.size() >= 2 ? ctx_ids[ctx_ids.size() - 2] : kNoWord;
  const double bi_total =
      (config_.use_context && last != kNoWord) ? static_cast<double>(model_.contextCount(last)) : 0.0;
  const double tri_total = (config_.use_context && last != kNoWord && prev != kNoWord)
                               ? static_cast<double>(model_.contextCount2(prev, last))
                               : 0.0;

  std::vector<Suggestion> out;
  out.reserve(candidates.size());
  for (const auto& [id, cand] : candidates) {
    const std::string& word = model_.word(id);
    if (word == kSentenceStart) continue;

    Suggestion s;
    s.word = word;
    s.sources = cand.sources;
    s.p_unigram = (static_cast<double>(model_.unigramCount(id)) + w.alpha) /
                  (total + w.alpha * vocab);
    if (bi_total > 0)
      s.p_bigram = static_cast<double>(model_.bigramCount(last, id)) / bi_total;
    if (tri_total > 0)
      s.p_trigram = static_cast<double>(model_.trigramCount(prev, last, id)) / tri_total;

    // Jelinek-Mercer: renormalise over the orders that actually exist, so a
    // missing trigram context does not silently shrink the whole probability.
    double lu = w.lambda_unigram, lb = 0.0, lt = 0.0;
    if (bi_total > 0) lb = w.lambda_bigram;
    if (tri_total > 0) lt = w.lambda_trigram;
    const double norm = lu + lb + lt;
    s.p_interp = (lu * s.p_unigram + lb * s.p_bigram + lt * s.p_trigram) / norm;

    s.phrase_boost = cand.phrase_hits > 0 ? std::log1p(cand.phrase_hits) : 0.0;
    s.recency_boost = (cand.sources & kFromRecent) ? recencyScore(id) : 0.0;

    const double prefix_fit =
        word.empty() ? 0.0
                     : static_cast<double>(norm_prefix.size()) / static_cast<double>(word.size());

    s.score = w.ngram * std::log(std::max(s.p_interp, 1e-12)) +
              w.phrase * s.phrase_boost + w.recency * s.recency_boost +
              w.prefix * prefix_fit;
    // A word that merely contains the query, without starting with it, is a
    // last resort rather than a real completion.
    if ((cand.sources & kFromSubstring) && !startsWith(word, norm_prefix))
      s.score += w.substring;

    out.push_back(std::move(s));
  }

  const size_t keep = std::min(k, out.size());
  std::partial_sort(out.begin(), out.begin() + static_cast<long>(keep), out.end(),
                    [](const Suggestion& a, const Suggestion& b) {
                      if (a.score != b.score) return a.score > b.score;
                      return a.word < b.word;
                    });
  out.resize(keep);
  return out;
}

void Engine::accept(std::string_view word) {
  const std::string w = normalize(word);
  if (w.empty()) return;

  session_ += w;
  session_ += ' ';
  trimSession();

  const uint32_t id = model_.id(w);
  if (id == kNoWord) return;

  auto it = lru_index_.find(id);
  if (it != lru_index_.end()) {
    lru_.erase(it->second);
    lru_index_.erase(it);
  }
  lru_.push_front(id);
  lru_index_[id] = lru_.begin();
  if (lru_.size() > config_.lru_capacity) {
    lru_index_.erase(lru_.back());
    lru_.pop_back();
  }
}

void Engine::observeText(std::string_view text) {
  for (const std::string& t : tokenize(text, /*mark_sentences=*/false)) {
    session_ += t;
    session_ += ' ';
  }
  trimSession();
}

// The phrase search is linear in the text it covers, so the session buffer is
// capped: older text stops influencing suggestions, which keeps every query
// O(window) no matter how long the document grows.
void Engine::trimSession() {
  if (config_.session_window == 0 || session_.size() <= config_.session_window) return;
  const size_t drop = session_.size() - config_.session_window;
  const size_t boundary = session_.find(' ', drop);
  session_.erase(0, boundary == std::string::npos ? drop : boundary);
  if (session_.empty() || session_.front() != ' ') session_.insert(session_.begin(), ' ');
}

void Engine::clearSession() {
  session_ = " ";
  lru_.clear();
  lru_index_.clear();
}

}  // namespace autocomplete
