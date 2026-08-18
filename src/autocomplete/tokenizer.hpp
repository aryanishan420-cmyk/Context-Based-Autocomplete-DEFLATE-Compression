// Turns raw text into the lower-cased word stream the n-gram model is built
// from.  Sentence boundaries matter for context, so they are preserved as an
// explicit start-of-sentence marker rather than being thrown away.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace autocomplete {

// Emitted at the start of every sentence so that "the" in "The dog barked" is
// predicted from a real context instead of from whatever ended the last line.
inline constexpr const char* kSentenceStart = "<s>";

// Splits into words of [a-z0-9'] (an apostrophe only counts inside a word, so
// "don't" stays whole but 'quoted' loses its quotes).
std::vector<std::string> tokenize(std::string_view text, bool mark_sentences = true);

// Normalises a single query word the same way the tokenizer would.
std::string normalize(std::string_view word);

// Splits a context string into its trailing words, most recent last.
std::vector<std::string> contextWords(std::string_view context, size_t max_words);

}  // namespace autocomplete
