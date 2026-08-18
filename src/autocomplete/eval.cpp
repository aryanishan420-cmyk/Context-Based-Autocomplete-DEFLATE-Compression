#include "autocomplete/eval.hpp"

#include <chrono>

#include "autocomplete/tokenizer.hpp"

namespace autocomplete {

EvalResult evaluate(const NgramModel& model, std::string_view test_text,
                    const EvalOptions& options) {
  EngineConfig cfg = options.engine;
  cfg.use_context = options.use_context;
  Engine engine(model, cfg);

  const std::vector<std::string> tokens = tokenize(test_text);

  EvalResult r;
  size_t hits1 = 0, hits3 = 0, hitsk = 0;
  double rr_sum = 0.0, saved = 0.0, typed = 0.0;
  std::string context;
  double total_us = 0.0;

  for (const std::string& word : tokens) {
    if (word == kSentenceStart) {
      context.clear();  // a new sentence starts from an empty context
      continue;
    }
    if (word.size() > options.prefix_len && r.queries < options.max_queries) {
      const std::string prefix = word.substr(0, options.prefix_len);

      const auto start = std::chrono::steady_clock::now();
      const std::vector<Suggestion> suggestions = engine.suggest(context, prefix, options.k);
      total_us += std::chrono::duration<double, std::micro>(
                      std::chrono::steady_clock::now() - start)
                      .count();

      size_t rank = 0;
      for (; rank < suggestions.size(); ++rank)
        if (suggestions[rank].word == word) break;

      ++r.queries;
      typed += static_cast<double>(word.size());
      if (rank < suggestions.size()) {
        rr_sum += 1.0 / static_cast<double>(rank + 1);
        if (rank < 1) {
          ++hits1;
          // A top-1 hit means the rest of the word never had to be typed.
          saved += static_cast<double>(word.size() - options.prefix_len);
        }
        if (rank < 3) ++hits3;
        ++hitsk;
      }
    }

    // Replay the real word: the engine's session state must follow the text.
    engine.accept(word);
    context += word;
    context += ' ';
  }

  if (r.queries) {
    const double q = static_cast<double>(r.queries);
    r.top1 = static_cast<double>(hits1) / q;
    r.top3 = static_cast<double>(hits3) / q;
    r.topk = static_cast<double>(hitsk) / q;
    r.mrr = rr_sum / q;
    r.avg_query_us = total_us / q;
    r.keystrokes_saved = typed > 0 ? saved / typed : 0.0;
  }
  return r;
}

}  // namespace autocomplete
