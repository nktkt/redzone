#!/usr/bin/env bash
#
# Per-function opt-out: REDZONE_NO_INSTRUMENT (clang's
# disable_sanitizer_instrumentation) excludes a function from access checking.
# The SAME out-of-bounds write is caught without the attribute and silently
# allowed with it -- proving the attribute, not the code, is what changes. The
# allocator is still redirected either way, so opting out can't corrupt the heap.
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

cat >"$TMP/optout.c" <<'EOF'
#include "redzone_rt.h"
#include <stdlib.h>
#ifdef OPT_OUT
#define MAYBE_OPT REDZONE_NO_INSTRUMENT
#else
#define MAYBE_OPT
#endif
// a[4] is one int past a 4-int region -> lands in the trailing red zone.
MAYBE_OPT static void touch(int *a) { a[4] = 99; }
int main(void) {
  int *a = malloc(4 * sizeof(int));
  if (!a)
    return 1;
  touch(a);
  free(a);
  return 0;
}
EOF

# Build optout.c with optional -DOPT_OUT ($1, may be empty) into $2.
build() {
  local def="$1" out="$2"
  clang -g -O0 -Iruntime $def -S -emit-llvm "$TMP/optout.c" -o "$TMP/o.ll" \
      2>>"$log" &&
    opt -load-pass-plugin="$PLUGIN" -passes=redzone -S "$TMP/o.ll" \
        -o "$TMP/o.instr.ll" 2>>"$log" &&
    clang -g "$TMP/o.instr.ll" runtime/redzone_rt.c -o "$out" 2>>"$log"
}

fail=0
pass() { printf 'PASS  %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }

# Without the attribute: the overflow must be caught.
if ! build "" "$TMP/prog_on"; then
  bad "build (no attribute)"; cat "$log"
else
  bash -c '"$0" >/dev/null 2>"$1"' "$TMP/prog_on" "$TMP/on.err" 2>/dev/null
  code=$?
  if [[ "$code" -ne 0 ]] && grep -qF "==redzone ERROR: heap-buffer-overflow" "$TMP/on.err"; then
    pass "without REDZONE_NO_INSTRUMENT: overflow is caught"
  else
    bad "without REDZONE_NO_INSTRUMENT: overflow is caught (exit=$code)"
    cat "$TMP/on.err"
  fi
fi

# With the attribute: the access isn't checked, so the program exits cleanly.
: >"$log"
if ! build "-DOPT_OUT" "$TMP/prog_off"; then
  bad "build (with attribute)"; cat "$log"
else
  if grep -q "1 opted-out fn(s)" "$log"; then
    pass "the pass reports the opted-out function"
  else
    bad "the pass reports the opted-out function"
  fi
  "$TMP/prog_off" >/dev/null 2>"$TMP/off.err"
  code=$?
  if [[ "$code" -eq 0 ]]; then
    pass "with REDZONE_NO_INSTRUMENT: access not checked (clean exit)"
  else
    bad "with REDZONE_NO_INSTRUMENT: access not checked (exit=$code)"
    cat "$TMP/off.err"
  fi
fi

echo
if [[ "$fail" -eq 0 ]]; then
  echo "opt-out tests passed."
else
  echo "opt-out tests FAILED." >&2
fi
exit "$fail"
