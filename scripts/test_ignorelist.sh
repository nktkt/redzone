#!/usr/bin/env bash
#
# File-level ignore-list: REDZONE_IGNORELIST names a file of `fun:<glob>` /
# `src:<glob>` rules that exclude matching functions / source files from access
# checking (for code you can't annotate). The SAME overflow is caught with no
# ignore-list and silently allowed when a rule matches, and a non-matching rule
# leaves it caught -- proving the rule, not the code, is what changes.
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
log="$TMP/build.log"

cat >"$TMP/ign_src.c" <<'EOF'
#include <stdlib.h>
// a[4] is one int past a 4-int region -> lands in the trailing red zone.
static void bad(int *a) { a[4] = 99; }
int main(void) {
  int *a = malloc(4 * sizeof(int));
  if (!a)
    return 1;
  bad(a);
  free(a);
  return 0;
}
EOF

# Build $TMP/ign_src.c with REDZONE_IGNORELIST=$1 (may be empty) into $2.
build() {
  local ignore="$1" out="$2"
  REDZONE_IGNORELIST="$ignore" \
    clang -g -O0 -S -emit-llvm "$TMP/ign_src.c" -o "$TMP/o.ll" 2>>"$log" &&
    REDZONE_IGNORELIST="$ignore" opt -load-pass-plugin="$PLUGIN" -passes=redzone \
        -S "$TMP/o.ll" -o "$TMP/o.instr.ll" 2>>"$log" &&
    clang -g "$TMP/o.instr.ll" runtime/redzone_rt.c -o "$out" 2>>"$log"
}

# Run a (possibly aborting) program, capturing stderr; echo its exit code.
run_capture() {
  bash -c '"$0" >/dev/null 2>"$1"' "$1" "$2" 2>/dev/null
  echo $?
}

fail=0
pass() { printf 'PASS  %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }

caught() { # build-arg, label, expect: yes=caught / no=clean
  local ignore="$1" label="$2" expect="$3" out="$TMP/prog" err="$TMP/run.err"
  : >"$log"
  if ! build "$ignore" "$out"; then
    bad "$label (build error)"; cat "$log"; return
  fi
  local code
  code="$(run_capture "$out" "$err")"
  if [[ "$expect" == yes ]]; then
    if [[ "$code" -ne 0 ]] && grep -qF "==redzone ERROR: heap-buffer-overflow" "$err"; then
      pass "$label"
    else
      bad "$label (exit=$code, expected the overflow to be caught)"; cat "$err"
    fi
  else
    if [[ "$code" -eq 0 ]]; then
      pass "$label"
    else
      bad "$label (exit=$code, expected a clean exit)"; cat "$err"
    fi
  fi
}

# No ignore-list: the overflow is caught.
caught "" "no ignore-list: overflow is caught" yes

# fun: rule matching bad() excludes it -> clean exit.
printf 'fun:bad\n' >"$TMP/fun.txt"
caught "$TMP/fun.txt" "fun:bad excludes the function (clean exit)" no

# src: rule matching the source file excludes the whole TU -> clean exit.
printf '# exclude this file\nsrc:*ign_src.c\n' >"$TMP/src.txt"
caught "$TMP/src.txt" "src:*ign_src.c excludes the file (clean exit)" no

# A non-matching rule leaves the overflow caught.
printf 'fun:does_not_match\n' >"$TMP/none.txt"
caught "$TMP/none.txt" "non-matching rule leaves the overflow caught" yes

echo
if [[ "$fail" -eq 0 ]]; then
  echo "ignore-list tests passed."
else
  echo "ignore-list tests FAILED." >&2
fi
exit "$fail"
