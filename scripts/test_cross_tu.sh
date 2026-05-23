#!/usr/bin/env bash
#
# Cross-translation-unit test for external-global instrumentation.
#
# redzone wraps a non-static global with a TRAILING red zone, keeping the data at
# the symbol's base so the address is unchanged. This test proves that ABI
# property the only way that matters: a SEPARATE, UN-instrumented translation
# unit references the same external symbol and must see the correct data (not the
# red zone). It also confirms an overflow of that global is still detected.
#
#   defs.c   (instrumented)     defines `shared[4]`, plus helpers to read it
#                               in-bounds and to overflow it.
#   driver.c (NOT instrumented) references `extern shared[4]` and reads it
#                               directly, proving the address is preserved.
#
# Run with no arg  -> valid: must print the expected sum and exit 0.
# Run with an arg  -> overflow: must abort with a global-buffer-overflow.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || llvm-config --prefix)"
PLUGIN="build/libRedzonePass.so"

if [[ ! -f "$PLUGIN" ]]; then
  echo "=== building redzone pass (plugin missing) ==="
  cmake -S . -B build -DCMAKE_PREFIX_PATH="$LLVM_PREFIX" >/dev/null
  cmake --build build >/dev/null
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# TU 1: defines the external global and instrumented helpers.
cat >"$TMP/defs.c" <<'EOF'
int shared[4] = {10, 20, 30, 40}; // external linkage; address must be preserved

int read_sum(void) { // instrumented in-bounds reads of the external global
  int s = 0;
  for (int i = 0; i < 4; i++)
    s += shared[i];
  return s;
}

void overflow_it(void) { shared[4] = 99; } // instrumented out-of-bounds write
EOF

# TU 2: references the same symbol from UN-instrumented code.
cat >"$TMP/driver.c" <<'EOF'
#include <stdio.h>
extern int shared[4];
extern int read_sum(void);
extern void overflow_it(void);

int main(int argc, char **argv) {
  long s = 0;
  for (int i = 0; i < 4; i++) // direct reads through the external symbol
    s += shared[i];
  s += read_sum();
  if (argc > 1)
    overflow_it();
  printf("%ld\n", s);
  return 0;
}
EOF

# Build defs.c WITH the pass; driver.c WITHOUT it; link with the runtime.
clang -g -O0 -Wno-array-bounds -S -emit-llvm "$TMP/defs.c" -o "$TMP/defs.ll" &&
  opt -load-pass-plugin="$PLUGIN" -passes=redzone -S "$TMP/defs.ll" \
      -o "$TMP/defs.instr.ll" &&
  clang -g -O0 -c "$TMP/driver.c" -o "$TMP/driver.o" &&
  clang -g "$TMP/defs.instr.ll" "$TMP/driver.o" runtime/redzone_rt.c \
      -o "$TMP/prog" || {
  echo "FAIL: cross-TU build failed"
  exit 1
}

fail=0

# 1. Valid run: the un-instrumented TU must read the right data.
#    shared = {10,20,30,40}; driver sums them (100) + read_sum() (100) = 200.
out="$("$TMP/prog" 2>/dev/null)"
code=$?
if [[ "$code" -eq 0 && "$out" == "200" ]]; then
  echo "PASS  cross-tu valid        (un-instrumented TU read shared correctly: $out)"
else
  echo "FAIL  cross-tu valid        (exit=$code, output='$out', expected '200')"
  fail=1
fi

# 2. Overflow run: writing shared[4] from the instrumented TU must be caught.
log="$TMP/err"
bash -c '"$0" trigger >/dev/null 2>"$1"' "$TMP/prog" "$log" 2>/dev/null
code=$?
if [[ "$code" -ne 0 ]] && grep -qF "==redzone ERROR: global-buffer-overflow" "$log"; then
  echo "PASS  cross-tu overflow     (detected global-buffer-overflow)"
else
  echo "FAIL  cross-tu overflow     (exit=$code; no global-buffer-overflow reported)"
  cat "$log"
  fail=1
fi

echo
if [[ "$fail" -eq 0 ]]; then
  echo "cross-TU external-global tests passed."
else
  echo "cross-TU external-global tests FAILED." >&2
fi
exit "$fail"
