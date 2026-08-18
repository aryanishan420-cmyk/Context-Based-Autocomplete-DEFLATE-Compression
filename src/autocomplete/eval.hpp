// Offline evaluation: replays held-out text one word at a time and measures
// how often the real next word appears in the top-k suggestions.
//
// The replay is sequential and accepts each true word as it goes, so the LRU
// and the KMP session index see the same history a real user would produce.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "autocomplete/engine.hpp"
#include "autocomplete/model.hpp"

namespace autocomplete {

struct EvalOptions {
  size_t prefix_len = 2;      // characters typed before asking for suggestions
  size_t k = 5;               // suggestions considered a "hit"
  size_t max_queries = 20000; // cap so a run stays quick
  bool use_context = true;    // false = unigram-only ablation
  EngineConfig engine;
};

struct EvalResult {
  size_t queries = 0;
  double top1 = 0.0;
  double top3 = 0.0;
  double topk = 0.0;
  double mrr = 0.0;              // mean reciprocal rank
  double keystrokes_saved = 0.0; // fraction of characters not typed on a top-1 hit
  double avg_query_us = 0.0;
};

EvalResult evaluate(const NgramModel& model, std::string_view test_text,
                    const EvalOptions& options);

}  // namespace autocomplete
