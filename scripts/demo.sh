#!/usr/bin/env bash
#
# Phase 1 demo: build the redzone pass + runtime, then run it on three
# examples — a valid program, a heap overflow, and a use-after-free.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || llvm-config --prefix)"
PLUGIN="build/libRedzonePass.so"

echo "=== configure & build pass ==="
cmake -S . -B build -DCMAKE_PREFIX_PATH="$LLVM_PREFIX" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
cmake --build build

# Compile a C source: emit IR, instrument it with the redzone pass, then link
# with the runtime (which is built WITHOUT the pass to avoid recursion).
instrument_and_build() {
  local src="$1" out="$2" name
  name="$(basename "$out")"
  clang -g -O0 -S -emit-llvm "$src" -o "build/${name}.ll"
  opt -load-pass-plugin="$PLUGIN" -passes=redzone -S "build/${name}.ll" \
      -o "build/${name}.instr.ll"
  clang -g "build/${name}.instr.ll" runtime/redzone_rt.c -o "$out"
}

echo "=== instrument examples ==="
instrument_and_build examples/valid.c          build/valid
instrument_and_build examples/heap_overflow.c  build/heap_overflow
instrument_and_build examples/use_after_free.c build/use_after_free

run() {
  echo
  echo "### $1"
  if "$1"; then
    echo "(exited cleanly)"
  else
    echo "(aborted — bug detected, as expected)"
  fi
}

run build/valid
run build/heap_overflow
run build/use_after_free
