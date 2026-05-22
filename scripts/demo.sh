#!/usr/bin/env bash
#
# Phase 0 demo: build the redzone pass and run it on examples/sample.c.
# The pass prints every load/store it observes (it does not instrument yet).
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || llvm-config --prefix)"

echo "=== configure & build ==="
cmake -S . -B build -DCMAKE_PREFIX_PATH="$LLVM_PREFIX" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build

echo "=== generate IR (with debug info) ==="
clang -g -O0 -S -emit-llvm examples/sample.c -o build/sample.ll

echo "=== run redzone pass ==="
opt -load-pass-plugin=build/libRedzonePass.so -passes=redzone \
    -disable-output build/sample.ll
