#!/usr/bin/env bash
#
# ccache compatibility check. Confirms that:
#   1. a redzone-instrumented compile is cacheable (second identical compile hits);
#   2. the cached object equals a fresh, un-cached compile (caching is correct);
#   3. changing a CCACHE_EXTRAFILES entry invalidates the cache.
#
# (3) is the safety property behind the recommended recipe: list the plugin in
# CCACHE_EXTRAFILES so that rebuilding it (a new redzone version) busts the cache.
# Without it ccache keys only on the plugin's *path*, and would serve stale
# objects produced by the old instrumentation. See docs/caching.md.
#
# Skipped (exit 0) when ccache isn't installed.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if ! command -v ccache >/dev/null 2>&1; then
  echo "SKIP  ccache not installed"
  exit 0
fi

LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || llvm-config --prefix)"
PLUGIN="$PWD/build/libRedzonePass.so"
if [[ ! -f "$PLUGIN" ]]; then
  cmake -S . -B build -DCMAKE_PREFIX_PATH="$LLVM_PREFIX" >/dev/null
  cmake --build build >/dev/null
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# A private cache so we never touch the developer's real one. The plugin copy is
# what we list in EXTRAFILES, so we can mutate it to test invalidation without
# disturbing the real plugin that -fpass-plugin actually loads.
export CCACHE_DIR="$TMP/cache"
export CCACHE_EXTRAFILES="$TMP/plugin-copy"
cp "$PLUGIN" "$TMP/plugin-copy"

SRC=examples/heap_overflow.c
CC=(ccache clang -O2 -g -fpass-plugin="$PLUGIN" -c "$SRC")

hits() { ccache --print-stats |
  awk -F'\t' '/^direct_cache_hit/{d=$2}/^preprocessed_cache_hit/{p=$2}END{print d+p}'; }
misses() { ccache --print-stats | awk -F'\t' '/^cache_miss/{print $2}'; }

fail=0
pass() { printf 'PASS  %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }

# Fresh, un-cached baseline for the correctness comparison.
clang -O2 -g -fpass-plugin="$PLUGIN" -c "$SRC" -o "$TMP/fresh.o" 2>/dev/null

"${CC[@]}" -o "$TMP/c1.o" 2>/dev/null # cold: a miss
"${CC[@]}" -o "$TMP/c2.o" 2>/dev/null # warm: should hit

if [[ "$(hits)" -ge 1 ]]; then
  pass "second instrumented compile is a cache hit"
else
  bad "second instrumented compile is a cache hit (hits=$(hits))"
fi

if [[ -s "$TMP/fresh.o" ]] && cmp -s "$TMP/fresh.o" "$TMP/c1.o" &&
  cmp -s "$TMP/c1.o" "$TMP/c2.o"; then
  pass "cached object matches a fresh un-cached compile"
else
  bad "cached object matches a fresh un-cached compile"
fi

# Mutate the extrafile -> the next compile must miss (cache invalidated).
m_before="$(misses)"
printf 'x' >>"$TMP/plugin-copy"
"${CC[@]}" -o "$TMP/c3.o" 2>/dev/null
if [[ "$(misses)" -gt "$m_before" ]]; then
  pass "CCACHE_EXTRAFILES change invalidates the cache"
else
  bad "CCACHE_EXTRAFILES change invalidates the cache (misses $m_before -> $(misses))"
fi

echo
if [[ "$fail" -eq 0 ]]; then
  echo "ccache compatibility check passed."
else
  echo "ccache compatibility check FAILED." >&2
fi
exit "$fail"
