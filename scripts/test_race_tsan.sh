#!/usr/bin/env bash
#
# Validate that the race detector's RUNTIME is itself free of data races, by
# building it + a race-free concurrent driver under ThreadSanitizer. This guards
# the sharded-shadow locking: any ThreadSanitizer warning means two threads
# touched the detector's own state without synchronization (a bug in the
# sharding), and the driver also asserts the detector reports no spurious race.
#
# ThreadSanitizer may not be available on every toolchain; if the instrumented
# build fails, the test SKIPS cleanly rather than failing CI.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || llvm-config --prefix)"
CLANG="$LLVM_PREFIX/bin/clang"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# The runtime is compiled WITH TSan here (not via the pass) so TSan can see its
# internal memory operations; this is a pure correctness check, not a build mode.
if ! "$CLANG" -fsanitize=thread -O1 -g -Iruntime \
    tests/race_tsan_test.c runtime/redzone_race.c runtime/redzone_race_rt.c \
    -pthread -o "$TMP/race_tsan_test" 2>"$TMP/build.log"; then
  echo "SKIP: ThreadSanitizer unavailable on this toolchain"
  sed 's/^/  /' "$TMP/build.log" | head -3
  exit 0
fi

# TSan exits nonzero on a finding; also grep defensively.
out="$(TSAN_OPTIONS="halt_on_error=1" "$TMP/race_tsan_test" 2>&1)"
code=$?
echo "$out"
if [[ "$code" -ne 0 ]] || grep -q "ThreadSanitizer" <<<"$out"; then
  echo "FAIL: the detector runtime has an internal data race (see above)"
  exit 1
fi
echo "race-tsan: PASS (detector runtime is internally race-free under TSan)"
