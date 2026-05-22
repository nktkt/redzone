#!/usr/bin/env bash
#
# redzone performance benchmark harness.
#
# Measures redzone's runtime overhead by building each program in bench/ two
# ways and timing them:
#   - baseline      (uninstrumented):  clang -O2 bench.c -o base
#   - instrumented:  clang -O2 -g -fpass-plugin=<plugin> -c bench.c -o b.o
#                    clang -O2 b.o redzone_rt_O2.o -o instr
#
# BOTH builds use -O2. An -O0 baseline is unrealistically slow and would
# inflate the slowdown ratio. The runtime (redzone_rt.c) is compiled ONCE at
# -O2 WITHOUT the plugin (compiling it with the plugin would rewrite its own
# malloc/free and recurse forever).
#
# Each benchmark is written to defeat the optimizer equally in both builds
# (argv-seeded trip count, volatile sink, data-dependent gather), so the
# baseline does the same real work the instrumented build can't elide.
#
# Methodology: each binary is run K times (default 5; override via the first
# CLI arg or the BENCH_RUNS env var) and we report the MIN wall-clock time,
# which is the most stable estimator of best-case throughput. A correctness
# check asserts the instrumented binary's stdout matches the baseline's.
#
# Usage:
#   ./scripts/bench.sh            # 5 runs each
#   ./scripts/bench.sh 9          # 9 runs each
#   BENCH_RUNS=3 ./scripts/bench.sh
#
# No artifacts are left in the repo: all binaries live in a temp dir that is
# removed on exit.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || llvm-config --prefix)"
PLUGIN="build/libRedzonePass.so"
RUNTIME="runtime/redzone_rt.c"

# Number of timed repetitions per binary (min is reported). First positional
# arg wins, else BENCH_RUNS, else 5.
RUNS="${1:-${BENCH_RUNS:-5}}"

# Per-benchmark workload argument (the iteration count passed on argv). Chosen
# so each baseline runs long enough to time reliably but the suite stays quick.
# A case statement (not an associative array) keeps this portable to the
# bash 3.2 that ships with macOS.
DEFAULT_WORKLOAD=4000
workload_for() {
  case "$1" in
    gather)      echo 40000 ;;
    alloc_churn) echo 100000 ;;
    compute)     echo 4000 ;;
    *)           echo "$DEFAULT_WORKLOAD" ;;
  esac
}

# Temp dir for all build/run artifacts; cleaned up on any exit.
TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

# --- 1. Ensure the pass plugin exists -------------------------------------
if [[ ! -f "$PLUGIN" ]]; then
  echo "=== building redzone pass (plugin missing) ==="
  cmake -S . -B build -DCMAKE_PREFIX_PATH="$LLVM_PREFIX" >/dev/null
  cmake --build build >/dev/null
fi

# --- 2. Compile the runtime once, at -O2, WITHOUT the plugin ---------------
RT_OBJ="$TMP/redzone_rt_O2.o"
echo "=== compiling runtime (-O2, no plugin) ==="
if ! clang -O2 -c "$RUNTIME" -o "$RT_OBJ" 2>"$TMP/rt.err"; then
  echo "ERROR: failed to compile runtime:" >&2
  cat "$TMP/rt.err" >&2
  exit 1
fi

# Build the two flavours of one benchmark source.
#   $1 = source path   $2 = baseline out   $3 = instrumented out
build_pair() {
  local src="$1" base="$2" instr="$3" obj
  obj="$TMP/$(basename "${src%.c}").o"
  # baseline: plain -O2, no plugin.
  clang -O2 "$src" -o "$base" 2>"$TMP/build.err" || return 1
  # instrumented: -O2 with the redzone pass plugin, then link the runtime.
  clang -O2 -g -fpass-plugin="$PLUGIN" -c "$src" -o "$obj" 2>>"$TMP/build.err" || return 1
  clang -O2 "$obj" "$RT_OBJ" -o "$instr" 2>>"$TMP/build.err" || return 1
}

# Time a command RUNS times and print the MIN wall time in milliseconds.
# Uses python3 for a high-resolution monotonic clock. Args: the full command.
time_min() {
  python3 - "$RUNS" "$@" <<'PY'
import subprocess, sys, time
runs = int(sys.argv[1])
cmd = sys.argv[2:]
best = None
for _ in range(runs):
    t0 = time.perf_counter()
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    dt = time.perf_counter() - t0
    if best is None or dt < best:
        best = dt
print(f"{best * 1000:.1f}")
PY
}

# --- 3. Build, check, and time every benchmark ----------------------------
printf '\n=== running benchmarks (min of %s runs, -O2 both) ===\n\n' "$RUNS"
printf '%-14s | %14s | %18s | %10s | %s\n' \
  "benchmark" "baseline (ms)" "instrumented (ms)" "slowdown" "correct"
printf -- '---------------+----------------+--------------------+------------+--------\n'

overall_ok=0

for src in bench/*.c; do
  [[ -e "$src" ]] || { echo "no benchmarks found in bench/" >&2; exit 1; }
  name="$(basename "${src%.c}")"
  work="$(workload_for "$name")"

  base="$TMP/${name}.base"
  instr="$TMP/${name}.instr"

  if ! build_pair "$src" "$base" "$instr"; then
    printf '%-14s | %s\n' "$name" "BUILD FAILED (see below)"
    cat "$TMP/build.err" >&2
    overall_ok=1
    continue
  fi

  # Correctness: instrumented stdout must equal baseline stdout. A mismatch
  # (or an abort from a spurious redzone error) means an instrumentation bug.
  base_out="$("$base" "$work" 2>/dev/null)"
  instr_out="$("$instr" "$work" 2>/dev/null)"
  if [[ "$base_out" == "$instr_out" ]]; then
    correct="OK"
  else
    correct="FAIL"
    overall_ok=1
  fi

  base_ms="$(time_min "$base" "$work")"
  instr_ms="$(time_min "$instr" "$work")"

  # slowdown = instrumented / baseline (computed in python for float math).
  slow="$(python3 -c "b=$base_ms; print(f'{($instr_ms/b):.1f}x' if b>0 else 'n/a')")"

  printf '%-14s | %14s | %18s | %10s | %s\n' \
    "$name" "$base_ms" "$instr_ms" "$slow" "$correct"
done

echo
if [[ "$overall_ok" -eq 0 ]]; then
  echo "all correctness checks passed."
else
  echo "WARNING: a correctness check or build FAILED (see above)." >&2
fi
exit "$overall_ok"
