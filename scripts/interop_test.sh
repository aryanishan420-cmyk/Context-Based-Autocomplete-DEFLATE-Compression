#!/usr/bin/env bash
# Cross-checks this DEFLATE implementation against the system gzip and, when
# available, Python's zlib.  The encoder is not required to produce the same
# bytes as any other tool -- only a stream that is valid per RFC 1951/1952,
# which is what "can gunzip read it?" actually proves.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin/deflate"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

[ -x "$BIN" ] || { echo "build first: make" >&2; exit 1; }

# Pick a real input if the corpus has been downloaded; otherwise synthesise one.
if [ -s "$ROOT/data/corpus.txt" ]; then
  head -c 2000000 "$ROOT/data/corpus.txt" > "$WORK/input"
else
  for i in $(seq 1 4000); do
    echo "line $i: the quick brown fox jumps over the lazy dog $((i * 7919))"
  done > "$WORK/input"
fi
head -c 300000 /dev/urandom > "$WORK/random"
printf '' > "$WORK/empty"
printf 'a' > "$WORK/single"

pass=0
fail=0
check() {
  if [ "$1" = "0" ]; then
    printf '  ok    %s\n' "$2"
    pass=$((pass + 1))
  else
    printf '  FAIL  %s\n' "$2"
    fail=$((fail + 1))
  fi
}

echo "our gzip output -> system gunzip"
for f in input random empty single; do
  for level in 1 6 9; do
    rc=0
    "$BIN" compress "$WORK/$f" --format gzip --level "$level" -o "$WORK/$f.gz" || rc=1
    gzip -t "$WORK/$f.gz" 2>/dev/null || rc=1
    gunzip -c "$WORK/$f.gz" > "$WORK/$f.out" 2>/dev/null || rc=1
    cmp -s "$WORK/$f.out" "$WORK/$f" || rc=1
    check "$rc" "$f at level $level survives gzip -t and gunzip"
  done
done

echo
echo "system gzip output -> our decoder"
for f in input random empty single; do
  for level in 1 6 9; do
    rc=0
    gzip -c -"$level" "$WORK/$f" > "$WORK/$f.ref.gz" || rc=1
    "$BIN" decompress "$WORK/$f.ref.gz" -o "$WORK/$f.ref.out" || rc=1
    cmp -s "$WORK/$f.ref.out" "$WORK/$f" || rc=1
    check "$rc" "gzip -$level output of $f decodes correctly"
  done
done

if command -v python3 > /dev/null 2>&1; then
  echo
  echo "zlib container, both directions (python3)"
  for f in input random single; do
    rc=0
    "$BIN" compress "$WORK/$f" --format zlib --level 9 -o "$WORK/$f.zz" || rc=1
    python3 -c "
import sys, zlib
data = zlib.decompress(open(sys.argv[1],'rb').read())
sys.exit(0 if data == open(sys.argv[2],'rb').read() else 1)
" "$WORK/$f.zz" "$WORK/$f" || rc=1
    check "$rc" "python zlib reads our zlib stream ($f)"

    rc=0
    python3 -c "
import sys, zlib
open(sys.argv[2],'wb').write(zlib.compress(open(sys.argv[1],'rb').read(), 9))
" "$WORK/$f" "$WORK/$f.py.zz" || rc=1
    "$BIN" decompress "$WORK/$f.py.zz" -o "$WORK/$f.py.out" || rc=1
    cmp -s "$WORK/$f.py.out" "$WORK/$f" || rc=1
    check "$rc" "we read python's zlib stream ($f)"
  done
fi

echo
echo "compression ratio next to system gzip (same input, level 9)"
ours=$("$BIN" compress "$WORK/input" --format gzip --level 9 -o "$WORK/ours.gz" && wc -c < "$WORK/ours.gz")
theirs=$(gzip -c -9 "$WORK/input" | wc -c)
raw=$(wc -c < "$WORK/input")
awk -v r="$raw" -v a="$ours" -v b="$theirs" 'BEGIN {
  printf "  input %d bytes | ours %d (%.4f) | gzip -9 %d (%.4f) | delta %+.2f%%\n",
         r, a, a/r, b, b/r, 100*(a-b)/b
}'

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
