#!/usr/bin/env bash
#
# End-to-end test for the data-race detector's RACE MODE: the pass emits the
# access/sync hooks automatically and the race runtime drives the happens-before
# state machine from real threads. See docs/design/data-race-detection.md.
#
# Flow per program: optimize to IR -> run the redzone pass with -redzone-race ->
# link the race runtime (compiled WITHOUT the pass) -> run. Each program is
# labeled "race" (must be flagged: nonzero exit + a "data race" report) or
# "clean" (must run with no report). Detection is timing-independent, so each is
# run several times as a flakiness gate.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || llvm-config --prefix)"
CLANG="$LLVM_PREFIX/bin/clang"
OPT="$LLVM_PREFIX/bin/opt"
PLUGIN="build/libRedzonePass.so"

# program (under examples/)        expected: race | clean
CASES=(
  "race_data_race.c:race"       # two unsynchronized writers
  "race_clean.c:clean"          # mutex-protected
  "race_rwlock_clean.c:clean"   # rwlock: readers + a writer, all locked
  "race_rwlock_unlocked.c:race" # locked writer + a rogue unlocked writer
  "race_trylock_clean.c:clean"  # mutual exclusion via mutex trylock
  "race_condvar_clean.c:clean"  # producer/consumer via cond_wait + timedwait
  "race_atomic_clean.c:clean"   # release/acquire flag publishing plain data
  "race_atomic_counter_clean.c:clean" # atomic fetch_add counter (RMW)
)

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
      "$TMP/$name.ll" -o "$TMP/$name.instr.ll" 2>/dev/null &&
    "$CLANG" -g "$TMP/$name.instr.ll" "${RT_OBJS[@]}" -pthread -o "$out"
}

RUNS="${REDZONE_RACE_RUNS:-10}"
fail=0

for entry in "${CASES[@]}"; do
  file="${entry%%:*}"
  expect="${entry#*:}"
  name="${file%.*}"
  bin="$TMP/$name"

  if ! build_race "examples/$file" "$bin" >/dev/null 2>"$TMP/$name.build.log"; then
    printf 'FAIL  %-24s (build failed)\n' "$file"
    cat "$TMP/$name.build.log"
    fail=1
    continue
  fi

  case_fail=0
  for i in $(seq 1 "$RUNS"); do
    out="$("$bin" 2>&1)"
    code=$?
    flagged=0
    { [[ "$code" -ne 0 ]] && grep -qF "data race" <<<"$out"; } && flagged=1
    if [[ "$expect" == "race" && "$flagged" -ne 1 ]]; then
      printf 'FAIL  %-24s (expected a race, none on run %d/%d, exit %d)\n' \
        "$file" "$i" "$RUNS" "$code"
      echo "$out"
      case_fail=1
      break
    fi
    if [[ "$expect" == "clean" && "$flagged" -ne 0 ]]; then
      printf 'FAIL  %-24s (false positive on run %d/%d)\n' "$file" "$i" "$RUNS"
      echo "$out"
      case_fail=1
      break
    fi
  done

  if [[ "$case_fail" -eq 0 ]]; then
    if [[ "$expect" == "race" ]]; then
      printf 'PASS  %-24s (race detected, %d/%d)\n' "$file" "$RUNS" "$RUNS"
    else
      printf 'PASS  %-24s (no race, %d/%d)\n' "$file" "$RUNS" "$RUNS"
    fi
  else
    fail=1
  fi
done

if [[ "$fail" -ne 0 ]]; then
  echo "race-e2e: FAILED"
  exit 1
fi
echo "race-e2e: all scenarios passed"
