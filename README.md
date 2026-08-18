# DSA Course Projects — Context-Based Autocomplete & DEFLATE Compression

Two data-structure/algorithm projects in C++17, built from scratch with no third-party
libraries, and validated against real data and real tools.

| | |
|---|---|
| **Autocomplete** | A trie with cached top-K completions, KMP string matching, and n-gram context scoring. Suggesting from context lifts top-1 accuracy from **34.4% → 48.8%** on held-out text. |
| **DEFLATE** | LZ77 sliding-window matching + length-limited Huffman coding, in the RFC 1951 block format. Output is read correctly by the system `gzip`, and lands within **0.12%** of `gzip -9`'s own compression ratio. |

Everything is measured on 8.7 MB of public-domain books downloaded from
[Project Gutenberg](https://www.gutenberg.org/) by `make data` — no data is committed to
this repository.

---

## Quick start

```bash
make            # builds bin/autocomplete, bin/deflate, bin/run_tests
make test       # 65 unit tests
make data       # downloads ~8.7 MB of public-domain books
make demo       # guided tour of both halves
make bench      # reproduces every number in this README
make interop    # checks our DEFLATE against the system gzip and python zlib
```

Requirements: a C++17 compiler and `make`. `curl` is needed for `make data`;
`gzip`/`python3` for `make interop`.

---

## Part 1 — Context-based autocomplete

```bash
bin/autocomplete build   --corpus data/train.txt --out data/model.bin
bin/autocomplete suggest --model data/model.bin --context "she could not" --prefix "he"
bin/autocomplete repl    --model data/model.bin
bin/autocomplete eval    --model data/model.bin --test data/test.txt --ablation
```

The same prefix resolves differently depending on what came before it:

```
$ bin/autocomplete suggest --context "the"     --prefix "c" -k 3
  1. case          2. count        3. carriage

$ bin/autocomplete suggest --context "the dog" --prefix "c" -k 3
  1. cart          2. came         3. comin
```

### How it works

A plain trie answers *"which words start with `he`?"*, but ranking them means walking the
whole subtree. This one **precomputes each node's top-K completions** in one post-order
pass, so a prefix query costs `O(|prefix|)` child lookups plus a copy of K entries —
independent of how many words share the prefix.

Candidates are gathered from four places and then scored by one function:

| Source | Structure | What it contributes |
|---|---|---|
| Prefix completions | trie with cached top-K | the frequent words starting with what was typed |
| Context successors | bigram/trigram maps | what usually follows the previous one or two words |
| Phrase repeats | **KMP** over the session text | what followed this exact phrase earlier *in this document* |
| Recent words | LRU (list + hash map) | what this user just used |

Scoring blends Jelinek–Mercer interpolated n-gram probability with the phrase, recency and
prefix-fit signals:

```
score(w) = log[ λ₃·P(w│w₋₂,w₋₁) + λ₂·P(w│w₋₁) + λ₁·P(w) ] / (λ₁+λ₂+λ₃)
         + 0.85·log(1 + phrase_hits)      ← KMP hits in the current document
         + 0.55·recency(w)                ← LRU rank
         + 0.35·|prefix|/|w|
```

**KMP earns its place twice.** The phrase search scans the session buffer for the last 2–3
words to find what followed them before, which is how suggestions adapt to the document
being written rather than only to the training corpus. And when a prefix search comes up
short, one KMP pass over the concatenated vocabulary finds mid-word matches in
`O(|vocab| + |query|)` — typing `ightning` still surfaces `lightning`.

### Results

Held-out text (8% of the corpus, never trained on), replayed word by word, 20 000 queries.
*Frequency only* is the same engine with context scoring switched off — a genuine ablation,
not a different program.

| Prefix typed | Ranking | top-1 | top-3 | top-5 | MRR | Keystrokes saved |
|---|---|---|---|---|---|---|
| 1 char | frequency only | 21.9% | 41.0% | 48.4% | 0.317 | 8.9% |
| 1 char | **context-based** | **37.7%** | **55.8%** | **62.4%** | **0.471** | **18.3%** |
| 2 chars | frequency only | 34.4% | 55.8% | 63.0% | 0.456 | 10.3% |
| 2 chars | **context-based** | **48.8%** | **66.5%** | **72.7%** | **0.582** | **18.7%** |
| 3 chars | frequency only | 47.3% | 71.5% | 79.7% | 0.599 | 14.8% |
| 3 chars | **context-based** | **59.3%** | **77.5%** | **83.6%** | **0.689** | **21.2%** |

Context is worth **+12 to +16 percentage points of top-1 accuracy** at every prefix length.

Model built from 8.1 MB of text in 2.6 s: 1.52 M tokens, 31 072-word vocabulary,
418 674 bigram types, 142 588 trigram types, 80 869 trie nodes. A query takes ~9 µs
without context and ~200 µs with it (the phrase search dominates, and is bounded by a
16 KiB session window so it stays `O(window)` however long the document grows).

---

## Part 2 — DEFLATE

```bash
bin/deflate compress   input.txt --format gzip --level 9 -o out.gz
bin/deflate decompress out.gz -o roundtrip.txt
bin/deflate inspect    input.txt --level 9     # block-by-block breakdown
bin/deflate bench      input.txt               # all ten levels
```

### How it works

**LZ77** slides a 32 KiB window over the input, finding repeats through hash chains on
3-byte prefixes. Levels 4–9 add *lazy matching*: a match is held back one byte to see
whether the next position starts a longer one, trading speed for ratio. Levels differ only
in chain length, "nice" match length and lazy threshold.

**Huffman** codes the resulting literal/length and distance streams. DEFLATE caps code
lengths at 15 bits, so instead of a plain Huffman tree that might grow deeper, this uses
the **package-merge algorithm** to build *length-limited optimal* codes. The unit tests
check it against unrestricted Huffman on the Fibonacci worst case, where the limit
genuinely binds.

**Block assembly** follows RFC 1951: for every block the encoder computes the exact bit
cost of all three block types — stored, fixed-Huffman, and dynamic with its RLE-coded
code-length alphabet — and emits the cheapest. That is why random data never inflates
(it falls back to stored blocks) and short blocks pick the fixed tables.

The decoder is a full inflater, strict about every check in the spec: Kraft inequality on
each code, stored-block length complements, distance bounds, CRC-32/Adler-32 trailers.

### Results

Whole 8.7 MB corpus, gzip container, i7-13650HX, g++ 13.3 `-O2`:

| Level | Output | Ratio | bits/byte | Compress | Decompress |
|---|---|---|---|---|---|
| 0 | 5 070 056 | 0.582 | 4.654 | 13.5 MiB/s | 19.5 MiB/s |
| 1 | 3 771 852 | 0.433 | 3.462 | 9.2 MiB/s | 25.6 MiB/s |
| 6 | 3 301 450 | 0.379 | 3.030 | 4.0 MiB/s | 27.9 MiB/s |
| 9 | 3 288 920 | 0.377 | 3.019 | 3.2 MiB/s | 36.0 MiB/s |

Against the system `gzip` on the same input:

| Level | Ours | `gzip` | Difference |
|---|---|---|---|
| 1 | 3 771 863 | 3 912 198 | **−3.59%** (ours is smaller) |
| 6 | 3 301 461 | 3 297 736 | +0.11% |
| 9 | 3 288 931 | 3 284 913 | +0.12% |

### Correctness

Matching `gzip`'s *ratio* is not the claim — producing a **valid stream** is, and that is
checked against tools that had no part in writing it (`make interop`, 30 checks):

- every file we produce passes `gzip -t` and decompresses correctly with `gunzip`
- every file `gzip -1/-6/-9` produces decompresses correctly with our decoder
- the same both ways for the zlib container against Python's `zlib`
- text, random (incompressible), single-byte and empty inputs all covered

---

## Testing

`make test` runs 65 unit tests with no external dependencies:

- **Huffman** — package-merge completeness (Kraft sum = 1), optimality against
  unrestricted Huffman, the 15-bit limit binding on Fibonacci weights, canonical codes
  round-tripping through the decoder, rejection of over- and under-subscribed codes, and
  the fixed tables checked against the literal values in RFC 1951
- **Bit I/O** — LSB-first packing and MSB-first Huffman codes, verified bit pattern by bit
  pattern against the spec
- **LZ77** — token replay reproduces the input exactly on randomised data at every level,
  window/length limits, overlapping-copy runs
- **DEFLATE** — round trips across all 10 levels × 3 containers × edge-case inputs,
  corrupted streams and bad checksums rejected, hand-written stored and fixed blocks decoded
- **Trie / KMP** — cached ranking verified against brute-force scans, KMP verified against
  naive search on 300 random strings, linear-time behaviour on the adversarial `aⁿ` case
- **Engine** — context changes the ranking, phrase search learns from the session, LRU
  promotion, substring fallback, and an ablation asserting context beats frequency

---

## Layout

```
src/deflate/       bitio, huffman (package-merge), lz77, deflate_encode, inflate, checksums
src/autocomplete/  tokenizer, trie, kmp, model (n-grams), engine (scoring), eval
src/apps/          the two command-line programs
tests/             65 unit tests and a small test framework
scripts/           fetch_data.sh, bench.sh, demo.sh, interop_test.sh
```

About 4 000 lines of C++17. No dependencies beyond the standard library.

---

## Data

Text is downloaded at build time from Project Gutenberg (public domain in the US); no book
text is stored in this repository.

Course project, Autumn 2024 — Data Structures & Algorithms (instructor: Ashutosh Kumar Gupta).
