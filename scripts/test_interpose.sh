#!/usr/bin/env bash
#
# EXPERIMENTAL whole-process malloc interposition (macOS). Builds the runtime and
# the interposer as dylibs, plus a test program split across two TUs:
#   - an UNINSTRUMENTED "library" allocator (compiled WITHOUT the pass), and
#   - an INSTRUMENTED main (compiled WITH the pass) that allocates via the library
#     and then accesses the block.
#
# Asserts the value proposition of interposition: an overflow on library-allocated
# memory is MISSED in a normal build (the library's malloc is untracked) but
# CAUGHT under DYLD_INSERT_LIBRARIES (the interposer red-zones it), and a correct
# program still runs clean. macOS-only (uses dyld's __interpose); skips elsewhere.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ "$(uname)" != "Darwin" ]]; then
  echo "SKIP: interposition test is macOS-only (dyld __interpose)"
  exit 0
fi

LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || llvm-config --prefix)"
CLANG="$LLVM_PREFIX/bin/clang"
OPT="$LLVM_PREFIX/bin/opt"
PLUGIN="build/libRedzonePass.so"

if [[ ! -f "$PLUGIN" ]]; then
  cmake -S . -B build -DCMAKE_PREFIX_PATH="$LLVM_PREFIX" >/dev/null
  cmake --build build >/dev/null
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Runtime + interposer as a shared dylib pair (one shadow, shared by both).
"$CLANG" -O2 -dynamiclib runtime/redzone_rt.c \
  -install_name @rpath/libredzone_rt.dylib -o "$TMP/libredzone_rt.dylib"
"$CLANG" -O2 -dynamiclib runtime/redzone_interpose.c -L"$TMP" -lredzone_rt \
  -Wl,-rpath,"$TMP" -install_name @rpath/libredzone_interpose.dylib \
  -o "$TMP/libredzone_interpose.dylib"
IP="$TMP/libredzone_interpose.dylib"

# Uninstrumented "library": its malloc is NOT redirected at compile time.
echo 'void *lib_alloc(unsigned long n){ return __builtin_malloc(n); }' >"$TMP/lib.c"
"$CLANG" -O0 -c "$TMP/lib.c" -o "$TMP/lib.o"

# Build an instrumented program from a source string. $1=src $2=out
build_instr() {
  "$CLANG" -O0 -g -S -emit-llvm "$1" -o "$1.ll" &&
    "$OPT" -load-pass-plugin="$PLUGIN" -passes=redzone -S "$1.ll" -o "$1.instr.ll" \
      2>/dev/null &&
    "$CLANG" -g "$1.instr.ll" "$TMP/lib.o" -L"$TMP" -lredzone_rt \
      -Wl,-rpath,"$TMP" -o "$2"
}

cat >"$TMP/over.c" <<'EOF'
#include <stdio.h>
extern void *lib_alloc(unsigned long);
int main(void){ char *p=(char*)lib_alloc(8); p[12]='X'; printf("%c\n",p[12]); return 0; }
EOF
cat >"$TMP/clean.c" <<'EOF'
#include <stdio.h>
extern void *lib_alloc(unsigned long);
int main(void){ char *p=(char*)lib_alloc(8); p[0]='r'; p[7]=0; printf("ok %c\n",p[0]); return 0; }
EOF
build_instr "$TMP/over.c" "$TMP/over" || { echo "FAIL: build over"; exit 1; }
build_instr "$TMP/clean.c" "$TMP/clean" || { echo "FAIL: build clean"; exit 1; }

fail=0

# 1. Baseline: library-allocated overflow is invisible without interposition.
out="$("$TMP/over" 2>&1)"; code=$?
if [[ "$code" -eq 0 ]] && ! grep -qF "redzone ERROR" <<<"$out"; then
  echo "PASS  baseline: library overflow not tracked (interposition needed)"
else
  echo "FAIL  baseline behaved unexpectedly (exit $code)"; fail=1
fi

# 2. With interposition: the same overflow is caught.
out="$(DYLD_INSERT_LIBRARIES="$IP" "$TMP/over" 2>&1)"; code=$?
if [[ "$code" -ne 0 ]] && grep -qF "heap-buffer-overflow" <<<"$out"; then
  echo "PASS  interpose: library overflow caught"
else
  echo "FAIL  interpose did not catch the overflow (exit $code)"; echo "$out" | head; fail=1
fi

# 3. With interposition: a correct program runs clean.
out="$(DYLD_INSERT_LIBRARIES="$IP" "$TMP/clean" 2>&1)"; code=$?
if [[ "$code" -eq 0 ]] && grep -qF "ok r" <<<"$out" &&
   ! grep -qF "redzone ERROR" <<<"$out"; then
  echo "PASS  interpose: correct program runs clean"
else
  echo "FAIL  interpose broke a correct program (exit $code)"; echo "$out" | head; fail=1
fi

if [[ "$fail" -ne 0 ]]; then echo "interpose: FAILED"; exit 1; fi
echo "interpose: all scenarios passed"
