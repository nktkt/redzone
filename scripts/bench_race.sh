#!/usr/bin/env bash
#
# Performance benchmark for the data-race detector's RACE MODE.
#
# For each program in bench/race/ it builds two flavours and times them:
#   - baseline      (uninstrumented):  clang -O1 src -pthread -o base
#   - instrumented  (race mode):       clang -O1 -emit-llvm | opt -redzone-race
#                                      | link the race runtime -pthread -o instr
# matching how `redzone --race` builds. The benchmarks are race-FREE (each
# thread touches only its own data), so the instrumented build must produce the
# same output as the baseline and must NOT report a race -- this doubles as a
# correctness gate. Perf numbers are printed for information (race mode is a
# heavy, opt-in mode and is not yet a hard CI perf gate).
#
# Reports the MIN wall time over RUNS repetitions (default 5; first arg or
# BENCH_RUNS overrides). Exits nonzero only on a build failure, an output
# mismatch, or a spurious race -- never on the timing itself.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || llvm-config --prefix)"
CLANG="$LLVM_PREFIX/bin/clang"
OPT="$LLVM_PREFIX/bin/opt"
PLUGIN="build/libRedzonePass.so"
RACE_RT=(runtime/redzone_race.c runtime/redzone_race_rt.c runtime/redzone_race_main.c)

RUNS="${1:-${BENCH_RUNS:-5}}"

workload_for() {
  case "$1" in
    race_serial)    echo 1000000 ;;
    race_contended) echo 200000 ;;
    *)              echo 200000 ;;
  esac
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [[ ! -f "$PLUGIN" ]]; then
  echo "=== building redzone pass (plugin missing) ==="
  cmake -S . -B build -DCMAKE_PREFIX_PATH="$LLVM_PREFIX" >/dev/null
  cmake --build build >/dev/null
fi

# Compile the race runtime once, at -O2, WITHOUT the plugin.
RT_OBJS=()
for f in "${RACE_RT[@]}"; do
  o="$TMP/$(basename "${f%.c}").o"
  "$CLANG" -O2 -c "$f" -o "$o" 2>"$TMP/rt.err" || {
    echo "ERROR: failed to compile $f"; cat "$TMP/rt.err"; exit 1; }
  RT_OBJS+=("$o")
done

# Time a command RUNS times; print the MIN wall time in milliseconds.
time_min() {
  python3 - "$RUNS" "$@" <<'PY'
import subprocess, sys, time
runs = int(sys.argv[1]); cmd = sys.argv[2:]
best = None
for _ in range(runs):
    t0 = time.perf_counter()
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    dt = time.perf_counter() - t0
    best = dt if best is None or dt < best else best
print(f"{best * 1000:.1f}")
PY
}

printf '\n=== race-mode benchmarks (min of %s runs, -O1 both) ===\n\n' "$RUNS"
printf '%-16s | %14s | %18s | %10s | %s\n' \
  "benchmark" "baseline (ms)" "instrumented (ms)" "slowdown" "correct"
printf -- '-----------------+----------------+--------------------+------------+--------\n'

status=0
for src in bench/race/*.c; do
  [[ -e "$src" ]] || { echo "no benchmarks in bench/race/" >&2; exit 1; }
  name="$(basename "${src%.c}")"
  work="$(workload_for "$name")"
  base="$TMP/$name.base"
  instr="$TMP/$name.instr"

  if ! "$CLANG" -O1 "$src" -pthread -o "$base" 2>"$TMP/b.err" ||
     ! "$CLANG" -O1 -g -S -emit-llvm "$src" -o "$TMP/$name.ll" 2>>"$TMP/b.err" ||
     ! "$OPT" -load-pass-plugin="$PLUGIN" -passes=redzone -redzone-race -S \
         "$TMP/$name.ll" -o "$TMP/$name.instr.ll" 2>>"$TMP/b.err" ||
     ! "$CLANG" -g "$TMP/$name.instr.ll" "${RT_OBJS[@]}" -pthread -o "$instr" \
         2>>"$TMP/b.err"; then
    printf '%-16s | BUILD FAILED\n' "$name"; cat "$TMP/b.err" >&2; status=1; continue
  fi

  # Correctness: instrumented output must match baseline AND not report a race.
  base_out="$("$base" "$work" 2>/dev/null)"
  instr_out="$("$instr" "$work" 2>"$TMP/$name.rerr")"; instr_code=$?
  if [[ "$base_out" == "$instr_out" ]] && [[ "$instr_code" -eq 0 ]] &&
     ! grep -qF "data race" "$TMP/$name.rerr"; then
    correct="OK"
  else
    correct="FAIL"; status=1
  fi

  base_ms="$(time_min "$base" "$work")"
  instr_ms="$(time_min "$instr" "$work")"
  slow="$(python3 -c "b=$base_ms; print(f'{($instr_ms/b):.1f}x' if b>0 else 'n/a')")"

  printf '%-16s | %14s | %18s | %10s | %s\n' \
    "$name" "$base_ms" "$instr_ms" "$slow" "$correct"
done

echo
if [[ "$status" -eq 0 ]]; then
  echo "race-bench: all correctness checks passed (perf is informational)."
else
  echo "race-bench: a build or correctness check FAILED (see above)." >&2
fi
exit "$status"
