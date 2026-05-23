#!/usr/bin/env bash
#
# Tests for redzone's rich reports:
#   * stack traces on errors (text and JSON), and
#   * leak suppressions via REDZONE_SUPPRESSIONS.
#
# set -e is deliberately off: buggy programs abort, and we inspect their status.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || llvm-config --prefix)"
PLUGIN="build/libRedzonePass.so"

if [[ ! -f "$PLUGIN" ]]; then
  cmake -S . -B build -DCMAKE_PREFIX_PATH="$LLVM_PREFIX" >/dev/null
  cmake --build build >/dev/null
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Instrument and link one C source into an executable.
build() {
  local src="$1" out="$2" name
  name="$(basename "$out")"
  clang -g -O0 -S -emit-llvm "$src" -o "$TMP/${name}.ll" &&
    opt -load-pass-plugin="$PLUGIN" -passes=redzone -S "$TMP/${name}.ll" \
        -o "$TMP/${name}.instr.ll" &&
    clang -g "$TMP/${name}.instr.ll" runtime/redzone_rt.c -o "$out"
}

# Run a (possibly aborting) program, capturing stderr to $2; echo its exit code.
run_capture() {
  bash -c '"$0" >/dev/null 2>"$1"' "$1" "$2" 2>/dev/null
  echo $?
}

fail=0
pass() { printf 'PASS  %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }

build examples/heap_overflow.c "$TMP/ho" 2>/dev/null || { echo "build failed"; exit 1; }
build examples/memory_leak.c "$TMP/ml" 2>/dev/null || { echo "build failed"; exit 1; }
build examples/multi_leak.c "$TMP/mleak" 2>/dev/null || { echo "build failed"; exit 1; }

# 1. A text error report carries a symbolized stack trace ending in main().
err="$TMP/ho.err"
code="$(run_capture "$TMP/ho" "$err")"
if [[ "$code" -ne 0 ]] && grep -q '#0' "$err" && grep -q 'main' "$err"; then
  pass "stack trace (text) on heap-buffer-overflow"
else
  bad "stack trace (text) on heap-buffer-overflow"; cat "$err"
fi

# 2. The JSON finding includes a non-empty "stack" array.
jerr="$TMP/ho.json"
REDZONE_FORMAT=json bash -c '"$0" >/dev/null 2>"$1"' "$TMP/ho" "$jerr" 2>/dev/null
if grep -q '"stack":\[' "$jerr" && python3 -c '
import json,sys
obj=json.loads(open(sys.argv[1]).read().splitlines()[0])
sys.exit(0 if obj.get("stack") else 1)' "$jerr"; then
  pass "stack trace (json) on heap-buffer-overflow"
else
  bad "stack trace (json) on heap-buffer-overflow"; cat "$jerr"
fi

# 3. Sanity: with no suppressions, the leak is reported (nonzero exit).
merr="$TMP/ml.err"
code="$(run_capture "$TMP/ml" "$merr")"
if [[ "$code" -ne 0 ]] && grep -q '==redzone ERROR: memory-leak' "$merr"; then
  pass "leak reported without suppressions"
else
  bad "leak reported without suppressions"; cat "$merr"
fi

# 4. A matching suppression silences the leak and the program exits clean.
echo "# silence the known leak in this example" >"$TMP/supp"
echo "leak:memory_leak.c" >>"$TMP/supp"
serr="$TMP/ml.supp.err"
REDZONE_SUPPRESSIONS="$TMP/supp" bash -c '"$0" >/dev/null 2>"$1"' "$TMP/ml" "$serr" 2>/dev/null
code=$?
if [[ "$code" -eq 0 ]] && ! grep -q 'memory-leak' "$serr"; then
  pass "leak suppressed by REDZONE_SUPPRESSIONS"
else
  bad "leak suppressed by REDZONE_SUPPRESSIONS (exit=$code)"; cat "$serr"
fi

# 5. A non-matching suppression leaves the leak reported.
echo "leak:does_not_match.c" >"$TMP/supp2"
nerr="$TMP/ml.nomatch.err"
code="$(REDZONE_SUPPRESSIONS="$TMP/supp2" run_capture "$TMP/ml" "$nerr")"
if [[ "$code" -ne 0 ]] && grep -q '==redzone ERROR: memory-leak' "$nerr"; then
  pass "non-matching suppression leaves leak reported"
else
  bad "non-matching suppression leaves leak reported"; cat "$nerr"
fi

# 6. Output is clean (no ANSI escapes) when stderr isn't a TTY (e.g. piped).
cerr="$TMP/ho.color.err"
run_capture "$TMP/ho" "$cerr" >/dev/null
if grep -q $'\x1b' "$cerr"; then
  bad "no color when piped (default)"
else
  pass "no color when piped (default)"
fi

# 7. REDZONE_COLOR=always forces ANSI escapes even when piped.
aerr="$TMP/ho.always.err"
REDZONE_COLOR=always bash -c '"$0" >/dev/null 2>"$1"' "$TMP/ho" "$aerr" 2>/dev/null
if grep -q $'\x1b' "$aerr"; then
  pass "REDZONE_COLOR=always emits color"
else
  bad "REDZONE_COLOR=always emits color"
fi

# 8. Leaks from one site are collapsed into a single counted line.
derr="$TMP/mleak.err"
code="$(run_capture "$TMP/mleak" "$derr")"
sites="$(grep -c 'allocation(s).*byte(s).*at ' "$derr")"
if [[ "$code" -ne 0 ]] && grep -q '5 allocation(s)' "$derr" && [[ "$sites" -eq 1 ]]; then
  pass "leaks deduplicated by allocation site"
else
  bad "leaks deduplicated by allocation site (site lines=$sites)"; cat "$derr"
fi

# 9. REDZONE_SYMBOLIZE upgrades trace frames to file:line (best-effort). Match a
#    frame line (#N ...) carrying the source file, not the always-present "at" line.
if command -v atos >/dev/null 2>&1 || command -v llvm-symbolizer >/dev/null 2>&1; then
  syerr="$TMP/ho.sym.err"
  REDZONE_SYMBOLIZE=1 bash -c '"$0" >/dev/null 2>"$1"' "$TMP/ho" "$syerr" 2>/dev/null
  if grep -qE '#[0-9]+ .*heap_overflow\.c:' "$syerr"; then
    pass "REDZONE_SYMBOLIZE adds file:line to stack frames"
  else
    bad "REDZONE_SYMBOLIZE adds file:line to stack frames"; cat "$syerr"
  fi
else
  echo "SKIP  REDZONE_SYMBOLIZE (no atos/llvm-symbolizer found)"
fi

echo
if [[ "$fail" -eq 0 ]]; then
  echo "report tests passed."
else
  echo "report tests FAILED." >&2
fi
exit "$fail"
