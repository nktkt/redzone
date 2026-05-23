#!/usr/bin/env bash
#
# Reproducible-build check: instrumenting the same source with the same flags
# must produce a byte-identical object file. This is the prerequisite for using
# redzone with a compiler cache (ccache/sccache) and for reproducible builds --
# if the pass were nondeterministic, a cache would serve stale or wrong objects.
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

# Compile $1 to $2 with the pass at -O2 -g. stderr goes to the log (never
# /dev/null) so a real failure is visible and never mistaken for "no output".
compile() {
  local src="$1" out="$2" ext flags
  ext="${src##*.}"
  flags="-O2 -g"
  [[ "$ext" == cpp ]] && flags="$flags -fno-exceptions -fno-rtti"
  clang $flags -fpass-plugin="$PLUGIN" -c "$src" -o "$out" 2>>"$log"
}

# A spread of sources: C and C++, with stack vars, globals, and allocators.
SOURCES=(
  examples/heap_overflow.c
  examples/global_overflow.c
  examples/extern_global_valid.c
  examples/cpp_new_valid.cpp
  bench/compute.c
  bench/alloc_churn.c
)

fail=0
for src in "${SOURCES[@]}"; do
  name="$(basename "${src%.*}")"
  if ! compile "$src" "$TMP/$name.1.o" || ! compile "$src" "$TMP/$name.2.o"; then
    printf 'FAIL  %-22s (build error; see below)\n' "$name"
    cat "$log"
    fail=1
    continue
  fi
  if [[ -s "$TMP/$name.1.o" ]] && cmp -s "$TMP/$name.1.o" "$TMP/$name.2.o"; then
    printf 'PASS  %-22s (byte-identical across two compiles)\n' "$name"
  else
    printf 'FAIL  %-22s (non-deterministic object output)\n' "$name"
    fail=1
  fi
done

echo
if [[ "$fail" -eq 0 ]]; then
  echo "determinism check passed: instrumented output is reproducible."
else
  echo "determinism check FAILED." >&2
fi
exit "$fail"
