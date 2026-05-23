#!/usr/bin/env bash
#
# sccache compatibility check. Confirms that:
#   1. a redzone-instrumented compile is cacheable (second identical compile hits);
#   2. the cached object equals a fresh, un-cached compile (caching is correct);
#   3. a different plugin *path* misses.
#
# (3) matters because sccache has no CCACHE_EXTRAFILES equivalent: it keys on the
# command line (which includes the plugin path) but not the plugin's contents. So
# the recommended recipe is to encode the plugin's version in its filename, so a
# new redzone produces a new command line and a clean miss. See docs/caching.md.
#
# Skipped (exit 0) when sccache isn't installed.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if ! command -v sccache >/dev/null 2>&1; then
  echo "SKIP  sccache not installed"
  exit 0
fi

LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || llvm-config --prefix)"
PLUGIN="$PWD/build/libRedzonePass.so"
if [[ ! -f "$PLUGIN" ]]; then
  cmake -S . -B build -DCMAKE_PREFIX_PATH="$LLVM_PREFIX" >/dev/null
  cmake --build build >/dev/null
fi

TMP="$(mktemp -d)"
# Private cache and a non-default server port, so we never disturb a developer's
# own sccache server or cache.
export SCCACHE_DIR="$TMP/sc"
export SCCACHE_CACHE_SIZE="1G"
export SCCACHE_SERVER_PORT="$((20000 + RANDOM % 20000))"
cleanup() {
  sccache --stop-server >/dev/null 2>&1 || true
  rm -rf "$TMP"
}
trap cleanup EXIT
sccache --stop-server >/dev/null 2>&1 || true
sccache --start-server >/dev/null 2>&1 || true
sccache --zero-stats >/dev/null 2>&1 || true

# Read totals from a captured stats dump (avoid piping into an early-exiting awk,
# which makes sccache complain about a broken pipe).
stat_val() { # $1=hits|misses
  local out
  out="$(sccache --show-stats 2>/dev/null)"
  echo "$out" |
    awk -v k="$1" '$1=="Cache"&&$2==k&&$3~/^[0-9]/{v=$3} END{print v+0}'
}
hits() { stat_val hits; }
misses() { stat_val misses; }

cp "$PLUGIN" "$TMP/plugin-v1.so"
cp "$PLUGIN" "$TMP/plugin-v2.so" # identical contents, different path = "new version"
SRC=examples/heap_overflow.c

fail=0
pass() { printf 'PASS  %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }

clang -O2 -g -fpass-plugin="$PLUGIN" -c "$SRC" -o "$TMP/fresh.o" 2>/dev/null

h0="$(hits)"
sccache clang -O2 -g -fpass-plugin="$TMP/plugin-v1.so" -c "$SRC" -o "$TMP/c1.o" 2>/dev/null
sccache clang -O2 -g -fpass-plugin="$TMP/plugin-v1.so" -c "$SRC" -o "$TMP/c2.o" 2>/dev/null
if [[ "$(hits)" -gt "$h0" ]]; then
  pass "second instrumented compile is a cache hit"
else
  bad "second instrumented compile is a cache hit (hits $h0 -> $(hits))"
fi

if [[ -s "$TMP/fresh.o" ]] && cmp -s "$TMP/fresh.o" "$TMP/c1.o" &&
  cmp -s "$TMP/c1.o" "$TMP/c2.o"; then
  pass "cached object matches a fresh un-cached compile"
else
  bad "cached object matches a fresh un-cached compile"
fi

m0="$(misses)"
sccache clang -O2 -g -fpass-plugin="$TMP/plugin-v2.so" -c "$SRC" -o "$TMP/c3.o" 2>/dev/null
if [[ "$(misses)" -gt "$m0" ]]; then
  pass "a different plugin path misses (version-in-path busts the cache)"
else
  bad "a different plugin path misses (misses $m0 -> $(misses))"
fi

echo
if [[ "$fail" -eq 0 ]]; then
  echo "sccache compatibility check passed."
else
  echo "sccache compatibility check FAILED." >&2
fi
exit "$fail"
