#!/usr/bin/env bash
#
# Unit test for the happens-before engine core (runtime/redzone_race.c),
# step 1 of the data-race detector (docs/design/data-race-detection.md).
#
# Pure logic, no plugin and no threads -- it just compiles the engine with a
# deterministic scenario driver and runs it. This validates the
# correctness-critical race decision in isolation before any of the surrounding
# instrumentation/runtime machinery is built.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if ! clang -O2 -Wall -Iruntime tests/race_engine_test.c runtime/redzone_race.c \
    -o "$TMP/race_engine_test" 2>"$TMP/build.log"; then
  echo "FAIL: race-engine test failed to build"
  cat "$TMP/build.log"
  exit 1
fi

"$TMP/race_engine_test"
