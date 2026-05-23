#!/usr/bin/env bash
#
# Real-thread test for the race-detector runtime glue (runtime/redzone_race_rt.c),
# step 1b of the data-race detector (docs/design/data-race-detection.md).
#
# It spins up actual pthreads through the runtime's wrappers, but its assertions
# are deterministic (happens-before is timing-independent). To turn that
# determinism into a regression gate, we run the binary many times and require
# every run to pass -- a single flaky run fails the suite.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

BIN="$TMP/race_runtime_test"
if ! clang -O2 -Wall -pthread -Iruntime \
    tests/race_runtime_test.c runtime/redzone_race.c runtime/redzone_race_rt.c \
    -o "$BIN" 2>"$TMP/build.log"; then
  echo "FAIL: race-runtime test failed to build"
  cat "$TMP/build.log"
  exit 1
fi

RUNS="${REDZONE_RACE_RUNS:-25}"
for i in $(seq 1 "$RUNS"); do
  if ! "$BIN" >"$TMP/run.log" 2>"$TMP/run.err"; then
    echo "FAIL: race-runtime test failed on run $i/$RUNS"
    cat "$TMP/run.log"
    cat "$TMP/run.err"
    exit 1
  fi
done

# Show the last run's scenario list, then confirm the repeat count.
cat "$TMP/run.log"
echo "race-runtime: $RUNS/$RUNS runs passed (deterministic)"
