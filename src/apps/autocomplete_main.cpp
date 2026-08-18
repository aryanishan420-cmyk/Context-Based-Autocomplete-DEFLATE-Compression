// Command line for the context-based autocomplete engine.
//
//   autocomplete build   --corpus data/train.txt --out data/model.bin
//   autocomplete suggest --model data/model.bin --context "it is a" --prefix "tru"
//   autocomplete repl    --model data/model.bin
//   autocomplete eval    --model data/model.bin --test data/test.txt
//   autocomplete stats   --model data/model.bin
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iomanip>

#include "autocomplete/engine.hpp"
#include "autocomplete/eval.hpp"
#include "autocomplete/model.hpp"
#include "autocomplete/tokenizer.hpp"
#include "common/cli.hpp"

using namespace autocomplete;

namespace {

void usage() {
  std::cout << R"(Context-based autocomplete (Trie + KMP + n-gram scoring)

usage: autocomplete <command> [options]

commands:
  build     Count n-grams over a corpus and write a model file
              --corpus FILE      training text (required)
              --out FILE         model output path (required)
              --min-count N      drop words seen fewer than N times (default 1)
              --min-trigram N    drop trigrams seen fewer than N times (default 2)

  suggest   Print ranked completions for one query
              --model FILE       model file (required)
              --context TEXT     words already typed (default empty)
              --prefix TEXT      characters of the current word (default empty)
              -k, --topk N       number of suggestions (default 5)
              --explain          show the score breakdown per suggestion
              --no-context       ignore context; rank by word frequency only

  repl      Interactive session; suggestions adapt as you type
              --model FILE       model file (required)
              --topk N           suggestions per query (default 5)

  eval      Replay held-out text and report top-k accuracy
              --model FILE       model file (required)
              --test FILE        held-out text (required)
              --prefix-len N     characters typed before suggesting (default 2)
              --topk N           k for the top-k metric (default 5)
              --max-queries N    cap on queries (default 20000)
              --ablation         also run a unigram-only pass for comparison

  stats     Print model and data-structure statistics
              --model FILE       model file (required)
)";
}

std::string require(const cli::Args& args, const char* name) {
  const std::string v = args.get(name);
  if (v.empty()) throw std::runtime_error(std::string("missing required --") + name);
  return v;
}

void printSuggestions(const std::vector<Suggestion>& suggestions, bool explain) {
  if (suggestions.empty()) {
    std::cout << "  (no suggestions)\n";
    return;
  }
  for (size_t i = 0; i < suggestions.size(); ++i) {
    const Suggestion& s = suggestions[i];
    std::cout << "  " << (i + 1) << ". " << std::left << std::setw(20) << s.word
              << std::right << std::fixed << std::setprecision(3) << s.score;
    if (explain) {
      std::cout << "   [" << s.sourceNames() << "]"
                << " P(uni)=" << std::scientific << std::setprecision(2) << s.p_unigram
                << " P(bi)=" << s.p_bigram << " P(tri)=" << s.p_trigram
                << " P(mix)=" << s.p_interp << std::fixed << std::setprecision(3)
                << " phrase=" << s.phrase_boost << " recent=" << s.recency_boost;
    }
    std::cout << "\n";
  }
}

int cmdBuild(const cli::Args& args) {
  const std::string corpus = require(args, "corpus");
  const std::string out = require(args, "out");

  BuildOptions opts;
  opts.min_word_count = static_cast<uint32_t>(args.num("min-count", 1));
  opts.min_trigram_count = static_cast<uint32_t>(args.num("min-trigram", 2));

  std::cout << "reading " << corpus << " ...\n";
  const std::string text = cli::readFile(corpus);

  const auto start = std::chrono::steady_clock::now();
  const NgramModel model = NgramModel::build(text, opts);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  model.save(out);
  const ModelStats s = model.stats();
  std::cout << "built in " << std::fixed << std::setprecision(2) << secs << " s from "
            << text.size() << " bytes\n"
            << "  tokens            " << s.tokens << "\n"
            << "  vocabulary        " << s.vocab << "\n"
            << "  bigram contexts   " << s.bigram_contexts << " (" << s.bigram_types << " types)\n"
            << "  trigram contexts  " << s.trigram_contexts << " (" << s.trigram_types << " types)\n"
            << "  trie nodes        " << s.trie_nodes << "\n"
            << "model written to " << out << "\n";
  return 0;
}

int cmdSuggest(const cli::Args& args) {
  const NgramModel model = NgramModel::load(require(args, "model"));
  EngineConfig cfg;
  cfg.use_context = !args.has("no-context");
  Engine engine(model, cfg);

  const std::string context = args.get("context");
  const std::string prefix = args.get("prefix");
  const size_t k = static_cast<size_t>(args.num("topk", args.num("k", 5)));

  if (!context.empty()) engine.observeText(context);

  const auto start = std::chrono::steady_clock::now();
  const std::vector<Suggestion> suggestions = engine.suggest(context, prefix, k);
  const double us = std::chrono::duration<double, std::micro>(
                        std::chrono::steady_clock::now() - start).count();

  std::cout << "context: \"" << context << "\"  prefix: \"" << prefix << "\"\n";
  printSuggestions(suggestions, args.has("explain"));
  std::cout << "  (" << std::fixed << std::setprecision(1) << us << " us)\n";
  return 0;
}

int cmdRepl(const cli::Args& args) {
  const NgramModel model = NgramModel::load(require(args, "model"));
  Engine engine(model);
  const size_t k = static_cast<size_t>(args.num("topk", 5));

  std::cout << "Type a sentence.  After each line the engine suggests completions\n"
               "for the last (partial) word, using the words before it as context.\n"
               "Commands: :reset clears the session, :quit exits.\n\n";

  std::string line;
  while (true) {
    std::cout << "> " << std::flush;
    if (!std::getline(std::cin, line)) break;
    if (line == ":quit" || line == ":q") break;
    if (line == ":reset") {
      engine.clearSession();
      std::cout << "session cleared\n";
      continue;
    }
    if (line.empty()) continue;

    // Everything but the last token is context; the last token is the prefix
    // being completed (a trailing space means "predict the next word").
    const bool ends_with_space = line.back() == ' ';
    const size_t split = line.find_last_of(' ');
    std::string context = (split == std::string::npos) ? "" : line.substr(0, split);
    std::string prefix = ends_with_space ? "" : (split == std::string::npos ? line : line.substr(split + 1));
    if (ends_with_space) context = line;

    for (const std::string& w : tokenize(context, false)) engine.accept(w);
    printSuggestions(engine.suggest(context, prefix, k), args.has("explain"));
  }
  return 0;
}

int cmdEval(const cli::Args& args) {
  const NgramModel model = NgramModel::load(require(args, "model"));
  const std::string test = cli::readFile(require(args, "test"));

  EvalOptions opts;
  opts.prefix_len = static_cast<size_t>(args.num("prefix-len", 2));
  opts.k = static_cast<size_t>(args.num("topk", 5));
  opts.max_queries = static_cast<size_t>(args.num("max-queries", 20000));

  auto row = [&](const char* label, const EvalResult& r) {
    std::cout << std::left << std::setw(16) << label << std::right << std::fixed
              << std::setprecision(1) << std::setw(9) << r.top1 * 100 << "%"
              << std::setw(9) << r.top3 * 100 << "%" << std::setw(9) << r.topk * 100 << "%"
              << std::setw(10) << std::setprecision(3) << r.mrr << std::setw(11)
              << std::setprecision(1) << r.keystrokes_saved * 100 << "%" << std::setw(11)
              << std::setprecision(1) << r.avg_query_us << " us\n";
  };

  std::cout << "queries: up to " << opts.max_queries << ", prefix length "
            << opts.prefix_len << ", k = " << opts.k << "\n\n"
            << std::left << std::setw(16) << "ranking" << std::right << std::setw(10)
            << "top-1" << std::setw(10) << "top-3" << std::setw(9) << "top-k"
            << std::setw(10) << "MRR" << std::setw(12) << "keystrokes" << std::setw(14)
            << "latency" << "\n"
            << std::string(81, '-') << "\n";

  opts.use_context = true;
  const EvalResult with_ctx = evaluate(model, test, opts);
  if (args.has("ablation")) {
    opts.use_context = false;
    row("frequency only", evaluate(model, test, opts));
  }
  row("context-based", with_ctx);
  std::cout << "\nqueries evaluated: " << with_ctx.queries << "\n";
  return 0;
}

int cmdStats(const cli::Args& args) {
  const NgramModel model = NgramModel::load(require(args, "model"));
  const ModelStats s = model.stats();
  std::cout << "vocabulary          " << s.vocab << " words\n"
            << "tokens counted      " << s.tokens << "\n"
            << "bigram contexts     " << s.bigram_contexts << "\n"
            << "bigram types        " << s.bigram_types << "\n"
            << "trigram contexts    " << s.trigram_contexts << "\n"
            << "trigram types       " << s.trigram_types << "\n"
            << "trie nodes          " << s.trie_nodes << "\n"
            << "trie cached entries " << s.trie_cached << "\n"
            << "trie memory (approx)" << std::fixed << std::setprecision(1)
            << static_cast<double>(s.trie_bytes) / (1024 * 1024) << " MiB\n";

  // A few sanity queries that show the structures answering real questions.
  const Engine engine(model);
  for (const char* prefix : {"th", "com", "beaut"}) {
    std::cout << "\ntop completions for \"" << prefix << "\": ";
    for (const Suggestion& sug : engine.suggest("", prefix, 5)) std::cout << sug.word << " ";
  }
  std::cout << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 1;
  }
  const std::string command = argv[1];
  const cli::Args args(argc, argv, 2);
  try {
    if (command == "build") return cmdBuild(args);
    if (command == "suggest") return cmdSuggest(args);
    if (command == "repl") return cmdRepl(args);
    if (command == "eval") return cmdEval(args);
    if (command == "stats") return cmdStats(args);
    if (command == "help" || command == "--help" || command == "-h") {
      usage();
      return 0;
    }
    std::cerr << "unknown command: " << command << "\n\n";
    usage();
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
