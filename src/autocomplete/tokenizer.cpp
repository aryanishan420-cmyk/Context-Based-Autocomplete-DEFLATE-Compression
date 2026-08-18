#include "autocomplete/tokenizer.hpp"

#include <cctype>

namespace autocomplete {
namespace {

inline bool isWordChar(unsigned char c) { return std::isalnum(c) != 0; }
inline bool isSentenceEnd(unsigned char c) { return c == '.' || c == '!' || c == '?'; }

}  // namespace

std::vector<std::string> tokenize(std::string_view text, bool mark_sentences) {
  std::vector<std::string> tokens;
  tokens.reserve(text.size() / 5 + 1);
  if (mark_sentences) tokens.emplace_back(kSentenceStart);

  std::string word;
  bool at_sentence_start = true;
  for (size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (isWordChar(c)) {
      word.push_back(static_cast<char>(std::tolower(c)));
    } else if (c == '\'' && !word.empty() && i + 1 < text.size() &&
               isWordChar(static_cast<unsigned char>(text[i + 1]))) {
      word.push_back('\'');  // internal apostrophe: don't, it's
    } else {
      if (!word.empty()) {
        tokens.push_back(word);
        word.clear();
        at_sentence_start = false;
      }
      if (mark_sentences && isSentenceEnd(c) && !at_sentence_start) {
        tokens.emplace_back(kSentenceStart);
        at_sentence_start = true;
      }
    }
  }
  if (!word.empty()) tokens.push_back(word);
  return tokens;
}

std::string normalize(std::string_view word) {
  std::string out;
  out.reserve(word.size());
  for (unsigned char c : word) {
    if (isWordChar(c))
      out.push_back(static_cast<char>(std::tolower(c)));
    else if (c == '\'' && !out.empty())
      out.push_back('\'');
  }
  return out;
}

std::vector<std::string> contextWords(std::string_view context, size_t max_words) {
  std::vector<std::string> words = tokenize(context, /*mark_sentences=*/false);
  if (words.size() > max_words)
    words.erase(words.begin(), words.end() - static_cast<long>(max_words));
  return words;
}

}  // namespace autocomplete
