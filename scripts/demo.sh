#!/usr/bin/env bash
# A short end-to-end tour of both halves of the project.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
DATA="$ROOT/data"
MODEL="$DATA/model.bin"

[ -x bin/autocomplete ] || { echo "run 'make' first" >&2; exit 1; }
[ -s "$DATA/train.txt" ] || { echo "run 'make data' first" >&2; exit 1; }
[ -s "$MODEL" ] || bin/autocomplete build --corpus "$DATA/train.txt" --out "$MODEL"

echo "=============================================================="
echo " 1. Autocomplete: the same prefix, three different contexts"
echo "=============================================================="
for ctx in "the" "the dog" "it is a truth universally"; do
  bin/autocomplete suggest --model "$MODEL" --context "$ctx" --prefix "ac" -k 3
  echo
done

echo "=============================================================="
echo " 2. Where each suggestion comes from"
echo "=============================================================="
bin/autocomplete suggest --model "$MODEL" --context "she could not" --prefix "he" -k 5 --explain
echo
echo "the same query with context switched off:"
bin/autocomplete suggest --model "$MODEL" --context "she could not" --prefix "he" -k 5 --no-context

echo
echo "=============================================================="
echo " 3. KMP substring fallback (no word starts with these letters)"
echo "=============================================================="
bin/autocomplete suggest --model "$MODEL" --context "" --prefix "ightning" -k 3 --explain

echo
echo "=============================================================="
echo " 4. DEFLATE: how a stream is actually built"
echo "=============================================================="
bin/deflate inspect "$DATA/books/alice-in-wonderland.txt" --level 9 | head -25

echo
echo "=============================================================="
echo " 5. DEFLATE: round trip through the system gzip"
echo "=============================================================="
IN="$DATA/books/alice-in-wonderland.txt"
bin/deflate compress "$IN" --format gzip --level 9 -o /tmp/demo.gz --stats
echo
echo "decompressing our file with the system gunzip:"
gunzip -c /tmp/demo.gz | cmp - "$IN" && echo "  identical to the original"
echo "decompressing the system gzip's file with our decoder:"
gzip -c -9 "$IN" > /tmp/demo_ref.gz
bin/deflate decompress /tmp/demo_ref.gz -o /tmp/demo_ref.txt
cmp /tmp/demo_ref.txt "$IN" && echo "  identical to the original"
rm -f /tmp/demo.gz /tmp/demo_ref.gz /tmp/demo_ref.txt
