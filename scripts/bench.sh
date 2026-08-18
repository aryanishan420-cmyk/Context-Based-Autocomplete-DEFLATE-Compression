#!/usr/bin/env bash
# Reproduces every number quoted in the README.
#   ./scripts/bench.sh            (needs `make` and `make data` first)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

DATA="${DATA_DIR:-$ROOT/data}"
CORPUS="$DATA/corpus.txt"
TRAIN="$DATA/train.txt"
TEST="$DATA/test.txt"
MODEL="$DATA/model.bin"

[ -x bin/deflate ] || { echo "run 'make' first" >&2; exit 1; }
[ -s "$CORPUS" ] || { echo "run 'make data' first" >&2; exit 1; }

hr() { printf '%s\n' "------------------------------------------------------------------------"; }

echo "corpus: $(wc -c < "$CORPUS") bytes from Project Gutenberg"
hr
echo "1. DEFLATE across levels (whole corpus, gzip container)"
bin/deflate bench "$CORPUS"

hr
echo "2. Ours vs system gzip, level for level"
printf '%-8s %14s %14s %10s\n' "level" "ours" "gzip" "delta"
raw=$(wc -c < "$CORPUS")
for level in 1 6 9; do
  ours=$(bin/deflate compress "$CORPUS" --format gzip --level "$level" -o /tmp/bench_ours.gz && wc -c < /tmp/bench_ours.gz)
  theirs=$(gzip -c -"$level" "$CORPUS" | wc -c)
  awk -v l="$level" -v a="$ours" -v b="$theirs" 'BEGIN {
    printf "%-8s %14d %14d %9.2f%%\n", l, a, b, 100*(a-b)/b }'
done
rm -f /tmp/bench_ours.gz
echo "(delta > 0 means our output is larger; both decode with either tool)"

hr
echo "3. Per-file compression, level 9"
printf '%-32s %10s %10s %8s\n' "file" "raw" "deflated" "ratio"
for f in "$DATA"/books/*.txt; do
  [ -s "$f" ] || continue
  raw=$(wc -c < "$f")
  out=$(bin/deflate compress "$f" --format raw --level 9 -o /tmp/bench_one.bin && wc -c < /tmp/bench_one.bin)
  awk -v n="$(basename "$f")" -v r="$raw" -v o="$out" 'BEGIN {
    printf "%-32s %10d %10d %8.4f\n", n, r, o, o/r }'
done
rm -f /tmp/bench_one.bin

hr
echo "4. Autocomplete: building the model"
bin/autocomplete build --corpus "$TRAIN" --out "$MODEL"

hr
echo "5. Autocomplete accuracy on held-out text (context vs frequency only)"
for plen in 1 2 3; do
  echo
  echo "prefix length $plen:"
  bin/autocomplete eval --model "$MODEL" --test "$TEST" --prefix-len "$plen" --ablation --max-queries 20000
done
