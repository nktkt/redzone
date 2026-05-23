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
#   ./scripts/bench.sh --check    # CI gate: fail if a regression budget is blown
#   ./scripts/bench.sh --check 7  # ...with 7 runs each
#
# No artifacts are left in the repo: all binaries live in a temp dir that is
# removed on exit.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || llvm-config --prefix)"
PLUGIN="build/libRedzonePass.so"
RUNTIME="runtime/redzone_rt.c"

# --check turns this into a CI regression gate: in addition to printing the
# table it asserts, per benchmark, that (1) the static instrumented-check count
# stays within budget and (2) the runtime slowdown stays under a ceiling, exiting
# nonzero if either is exceeded (alongside the existing correctness check).
CHECK=0
case "${1:-}" in
  --check) CHECK=1; shift ;;
esac

# Number of timed repetitions per binary (min is reported). First positional
# arg wins, else BENCH_RUNS, else 5.
RUNS="${1:-${BENCH_RUNS:-5}}"

# Regression thresholds for --check, per benchmark. These are deliberately loose
# tripwires, not precise targets: sized to ride out normal CI noise yet catch
# order-of-magnitude regressions (e.g. disabling selective instrumentation, which
# balloons the check count ~10x, or an O(n^2) allocator, which sends alloc_churn
# back to hundreds-of-x). Tighten only if CI proves stable enough.
#
#   budget_for  = max static __redzone_check sites the pass may emit (count is
#                 deterministic given the compiler, so this gate never flakes).
#   ceiling_for = max slowdown (instrumented/baseline). The runtime ratio is
#                 hardware-independent (both builds run on the same machine);
#                 alloc_churn's is the noisiest (its baseline is a few ms), hence
#                 the widest ceiling.
budget_for() {
  case "$1" in
    compute) echo 16 ;; gather) echo 16 ;; alloc_churn) echo 16 ;; *) echo 32 ;;
  esac
}
ceiling_for() {
  case "$1" in
    compute) echo 5 ;; gather) echo 40 ;; alloc_churn) echo 100 ;; *) echo 50 ;;
  esac
}

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

  # Static check count the pass emitted (printed to stderr during the
  # instrumented compile, captured in build.err). Deterministic; gated by --check.
  checks="$(grep -oE 'instrumented [0-9]+' "$TMP/build.err" | grep -oE '[0-9]+' | head -1)"

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

  # --check: assert the count and slowdown gates for this benchmark.
  if [[ "$CHECK" -eq 1 ]]; then
    budget="$(budget_for "$name")"
    ceil="$(ceiling_for "$name")"
    if [[ -n "$checks" ]] && [[ "$checks" -le "$budget" ]]; then
      cgate="ok"
    else
      cgate="FAIL"; overall_ok=1
    fi
    if [[ "$slow" == "n/a" ]]; then
      rgate="skip" # baseline too fast to time; ratio gate not meaningful
    elif [[ "$(python3 -c "print(1 if float('${slow%x}') > $ceil else 0)")" -eq 1 ]]; then
      rgate="FAIL"; overall_ok=1
    else
      rgate="ok"
    fi
    printf '   gate: checks %s/%s [%s], slowdown %s/%sx [%s]\n' \
      "${checks:-?}" "$budget" "$cgate" "$slow" "$ceil" "$rgate"
  fi
done

echo
if [[ "$overall_ok" -eq 0 ]]; then
  if [[ "$CHECK" -eq 1 ]]; then
    echo "all correctness checks and regression gates passed."
  else
    echo "all correctness checks passed."
  fi
else
  echo "WARNING: a correctness check, build, or regression gate FAILED (see above)." >&2
fi
exit "$overall_ok"
