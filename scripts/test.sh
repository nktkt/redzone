#!/usr/bin/env bash
#
# redzone test runner: builds the pass (if needed), instruments each example,
# runs it, and asserts the observed outcome matches the expected one.
#
# Each entry in the CASES table is "example_file:expected", where expected is
# either "OK" (the program must exit 0) or a redzone error-kind string (the
# program must exit nonzero AND print "==redzone ERROR: <kind>" to stderr).
#
# NOTE: we deliberately do NOT use `set -e` — buggy examples are *supposed* to
# abort with a nonzero status, and we inspect those statuses by hand.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || llvm-config --prefix)"
PLUGIN="build/libRedzonePass.so"

# example file (under examples/)      expected outcome
CASES=(
  "valid.c:OK"
  "big_valid.c:OK"
  "two_allocs_valid.c:OK"
  "heap_overflow.c:heap-buffer-overflow"
  "off_by_one_read.c:heap-buffer-overflow"
  "underflow_write.c:heap-buffer-overflow"
  "use_after_free.c:use-after-free"
  "use_after_free_write.c:use-after-free"
  "double_free.c:double-free"
  "invalid_free.c:invalid-free"
)

# Build the pass plugin only if it isn't already present.
if [[ ! -f "$PLUGIN" ]]; then
  echo "=== building redzone pass (plugin missing) ==="
  cmake -S . -B build -DCMAKE_PREFIX_PATH="$LLVM_PREFIX" >/dev/null
  cmake --build build >/dev/null
fi

# Compile a C source: emit IR, instrument it with the redzone pass, then link
# with the runtime (which is built WITHOUT the pass to avoid recursion).
# Intermediate artifacts all live under build/.
instrument_and_build() {
  local src="$1" out="$2" name
  name="$(basename "$out")"
  clang -g -O0 -S -emit-llvm "$src" -o "build/${name}.ll" &&
    opt -load-pass-plugin="$PLUGIN" -passes=redzone -S "build/${name}.ll" \
        -o "build/${name}.instr.ll" &&
    clang -g "build/${name}.instr.ll" runtime/redzone_rt.c -o "$out"
}

pass=0
total=0

for entry in "${CASES[@]}"; do
  file="${entry%%:*}"
  expected="${entry#*:}"
  total=$((total + 1))

  src="examples/${file}"
  name="${file%.c}"
  bin="build/${name}"
  log="build/${name}.stderr"

  if [[ ! -f "$src" ]]; then
    printf 'FAIL  %-24s (missing source %s)\n' "$file" "$src"
    continue
  fi

  if ! instrument_and_build "$src" "$bin" >/dev/null 2>"$log"; then
    printf 'FAIL  %-24s (build failed; see %s)\n' "$file" "$log"
    continue
  fi

  # Run via an inner bash -c so that the SIGABRT death of a buggy example is
  # reaped by that inner shell. The example's own stderr is captured to "$log"
  # (fd redirected before the inner shell's job-control "Abort trap" notice can
  # reach it), and the inner shell's stderr is discarded so that notice never
  # reaches our output. The inner shell still propagates the child's exit
  # status (128 + signo for a signal death).
  bash -c '"$0" >/dev/null 2>"$1"' "$bin" "$log" 2>/dev/null
  code=$?

  if [[ "$expected" == "OK" ]]; then
    if [[ "$code" -eq 0 ]]; then
      printf 'PASS  %-24s (clean exit)\n' "$file"
      pass=$((pass + 1))
    else
      printf 'FAIL  %-24s (expected clean exit, got exit %d)\n' "$file" "$code"
    fi
  else
    needle="==redzone ERROR: ${expected}"
    if [[ "$code" -ne 0 ]] && grep -qF "$needle" "$log"; then
      printf 'PASS  %-24s (detected %s)\n' "$file" "$expected"
      pass=$((pass + 1))
    elif [[ "$code" -eq 0 ]]; then
      printf 'FAIL  %-24s (expected %s, but exited cleanly)\n' "$file" "$expected"
    else
      printf 'FAIL  %-24s (exited %d but no "%s" on stderr)\n' "$file" "$code" "$needle"
    fi
  fi
done

echo
echo "${pass}/${total} passed"
[[ "$pass" -eq "$total" ]]
