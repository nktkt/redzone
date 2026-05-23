#!/usr/bin/env bash
#
# End-to-end test for the data-race detector's RACE MODE (step 1b complete):
# the pass emits the access/sync hooks automatically and the race runtime drives
# the happens-before state machine from real threads. See
# docs/design/data-race-detection.md.
#
# Flow per program: optimize to IR -> run the redzone pass with -redzone-race ->
# link the race runtime (compiled WITHOUT the pass) -> run. A racy program must
# be flagged (nonzero exit + a "data race" report); a correctly-synchronized one
# must run clean. Both are timing-independent, so each is run several times as a
# flakiness gate.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || llvm-config --prefix)"
CLANG="$LLVM_PREFIX/bin/clang"
OPT="$LLVM_PREFIX/bin/opt"
PLUGIN="build/libRedzonePass.so"

if [[ ! -f "$PLUGIN" ]]; then
  echo "=== building redzone pass (plugin missing) ==="
  cmake -S . -B build -DCMAKE_PREFIX_PATH="$LLVM_PREFIX" >/dev/null
  cmake --build build >/dev/null
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Race runtime, compiled once WITHOUT the pass (instrumenting it would recurse).
"$CLANG" -O2 -c runtime/redzone_race.c -o "$TMP/redzone_race.o"
"$CLANG" -O2 -c runtime/redzone_race_rt.c -o "$TMP/redzone_race_rt.o"
"$CLANG" -O2 -c runtime/redzone_race_main.c -o "$TMP/redzone_race_main.o"
RT_OBJS=("$TMP/redzone_race.o" "$TMP/redzone_race_rt.o" "$TMP/redzone_race_main.o")

# Build an instrumented race-mode binary from a C source.
build_race() {
  local src="$1" out="$2" name
  name="$(basename "$out")"
  # Optimize first (so promotable locals are in registers and only real memory
  # accesses remain), THEN run the pass in race mode, THEN link the runtime.
  "$CLANG" -O1 -g -S -emit-llvm "$src" -o "$TMP/$name.ll" &&
    "$OPT" -load-pass-plugin="$PLUGIN" -passes=redzone -redzone-race -S \
      "$TMP/$name.ll" -o "$TMP/$name.instr.ll" &&
    "$CLANG" -g "$TMP/$name.instr.ll" "${RT_OBJS[@]}" -pthread -o "$out"
}

RUNS="${REDZONE_RACE_RUNS:-10}"
fail=0

# --- Racy program: must be flagged on every run. ---
if ! build_race examples/race_data_race.c "$TMP/race_data_race"; then
  echo "FAIL: race_data_race failed to build"
  exit 1
fi
for i in $(seq 1 "$RUNS"); do
  out="$("$TMP/race_data_race" 2>&1)"
  code=$?
  if [[ "$code" -eq 0 ]] || ! grep -qF "data race" <<<"$out"; then
    echo "FAIL: race_data_race not flagged on run $i/$RUNS (exit $code)"
    echo "$out"
    fail=1
    break
  fi
done
[[ "$fail" -eq 0 ]] && echo "PASS  race_data_race -> race detected ($RUNS/$RUNS)"

# --- Clean program: must run silently on every run (no false positive). ---
if ! build_race examples/race_clean.c "$TMP/race_clean"; then
  echo "FAIL: race_clean failed to build"
  exit 1
fi
for i in $(seq 1 "$RUNS"); do
  out="$("$TMP/race_clean" 2>&1)"
  code=$?
  if [[ "$code" -ne 0 ]] || grep -qF "data race" <<<"$out"; then
    echo "FAIL: race_clean false-flagged on run $i/$RUNS (exit $code)"
    echo "$out"
    fail=1
    break
  fi
done
[[ "$fail" -eq 0 ]] && echo "PASS  race_clean      -> no race ($RUNS/$RUNS)"

if [[ "$fail" -ne 0 ]]; then
  echo "race-e2e: FAILED"
  exit 1
fi
echo "race-e2e: all scenarios passed"
