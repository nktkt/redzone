#!/usr/bin/env bash
#
# Differential test against AddressSanitizer (the gold-standard detector).
#
# For a curated subset of the example corpus, this builds each program with
# `-fsanitize=address` and checks ASan's verdict (clean vs. the expected bug
# kind). redzone's own verdict on these same programs is locked down by
# scripts/test.sh, so agreement here means redzone and ASan reach the SAME
# conclusion -- catching the same bugs and staying clean on the same valid code.
#
# Scope: single-file C programs whose bug class ASan also detects. Excluded:
# memory leaks (macOS has no LeakSanitizer), cross-TU globals (multi-file build),
# C++ and threaded examples (kept out for a simple, deterministic comparison),
# and strlcpy/strlcat -- macOS ASan has no interceptor for them, so it MISSES
# `strlcpy_overflow.c` (which redzone catches), making ASan unusable as the
# reference there. That case is a small example of redzone being more thorough
# than macOS ASan; its detection is still gated by scripts/test.sh.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || llvm-config --prefix)"
CLANG="$LLVM_PREFIX/bin/clang"

# example : expected ASan verdict.
#   OK   = must exit cleanly.
#   ANY  = must report some AddressSanitizer error (kind wording differs).
#   else = AddressSanitizer must report exactly this kind.
# (redzone's "use-after-free" is ASan's "heap-use-after-free"; same bug.)
CASES=(
  "valid.c:OK"
  "big_valid.c:OK"
  "two_allocs_valid.c:OK"
  "realloc_grow.c:OK"
  "heap_overflow.c:heap-buffer-overflow"
  "off_by_one_read.c:heap-buffer-overflow"
  "underflow_write.c:heap-buffer-overflow"
  "calloc_overflow.c:heap-buffer-overflow"
  "use_after_free.c:heap-use-after-free"
  "use_after_free_write.c:heap-use-after-free"
  "double_free.c:ANY"
  "invalid_free.c:ANY"
  "stack_overflow.c:stack-buffer-overflow"
  "global_overflow.c:global-buffer-overflow"
  "memcpy_overflow.c:heap-buffer-overflow"
  "memset_overflow.c:heap-buffer-overflow"
  "memcpy_valid.c:OK"
  "strcpy_overflow.c:heap-buffer-overflow"
  "strcat_overflow.c:heap-buffer-overflow"
  "strncpy_overflow.c:heap-buffer-overflow"
  "str_valid.c:OK"
  "sprintf_overflow.c:heap-buffer-overflow"
  "snprintf_overflow.c:heap-buffer-overflow"
  "printf_valid.c:OK"
)

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0
total=0
for entry in "${CASES[@]}"; do
  file="${entry%%:*}"
  exp="${entry#*:}"
  total=$((total + 1))
  src="examples/$file"
  bin="$TMP/${file%.c}"

  if ! "$CLANG" -fsanitize=address -g "$src" -o "$bin" 2>"$TMP/build.log"; then
    printf 'FAIL  %-24s (ASan build failed)\n' "$file"
    continue
  fi

  out="$(ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 "$bin" 2>&1)"
  code=$?

  if [[ "$exp" == "OK" ]]; then
    if [[ "$code" -eq 0 ]]; then
      printf 'PASS  %-24s (ASan: clean, agrees)\n' "$file"
      pass=$((pass + 1))
    else
      printf 'FAIL  %-24s (expected clean, ASan reported an error)\n' "$file"
    fi
    continue
  fi

  # Expect an error.
  if [[ "$code" -eq 0 ]] || ! grep -q "AddressSanitizer" <<<"$out"; then
    printf 'FAIL  %-24s (expected a bug, ASan was clean)\n' "$file"
    continue
  fi
  if [[ "$exp" == "ANY" ]] || grep -qF "AddressSanitizer: $exp" <<<"$out"; then
    printf 'PASS  %-24s (ASan: %s, agrees)\n' "$file" \
      "$([[ "$exp" == "ANY" ]] && echo error || echo "$exp")"
    pass=$((pass + 1))
  else
    printf 'FAIL  %-24s (expected %s, ASan reported a different kind)\n' \
      "$file" "$exp"
    grep -o "AddressSanitizer: [a-z-]*" <<<"$out" | head -1
  fi
done

echo
echo "diff-asan: $pass/$total agreed with AddressSanitizer"
[[ "$pass" -eq "$total" ]]
