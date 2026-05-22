#!/usr/bin/env bash
#
# redzone machine-readable output tests.
#
# Builds each example with `./scripts/redzone build` into a throwaway temp dir,
# runs the instrumented binary under REDZONE_FORMAT=json and REDZONE_FORMAT=sarif,
# and validates the findings emitted on *stderr* with python3:
#
#   json:  ONE JSON object per line (JSON-Lines). Every nonempty stderr line must
#          parse as a standalone JSON object, and at least one must carry the
#          expected error-kind in its "error" field.
#   sarif: the WHOLE stderr is a single SARIF 2.1.0 document; we check
#          version == "2.1.0" and runs[0].results[0].ruleId == the expected kind.
#
# We also confirm a clean program (valid.c) emits NO findings (empty stderr,
# exit 0) in json mode.
#
# NOTE: NOT `set -e` — buggy examples are *supposed* to abort with a nonzero
# status, so we inspect outcomes by hand rather than letting a nonzero exit
# kill the script.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Make Homebrew LLVM's clang/opt visible to ./scripts/redzone, which calls them
# unqualified. (The redzone CLI discovers the LLVM prefix for *building the
# plugin*, but the instrument/link steps invoke clang/opt from PATH.)
LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || llvm-config --prefix 2>/dev/null || true)"
if [[ -n "$LLVM_PREFIX" && -d "$LLVM_PREFIX/bin" ]]; then
  export PATH="$LLVM_PREFIX/bin:$PATH"
fi

# All build artifacts and captured findings live here; wiped on exit so the
# repo root never accumulates stray binaries.
TMP="$(mktemp -d "${TMPDIR:-/tmp}/redzone-fmt.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

pass=0
total=0

# Record a check result. $1 = PASS|FAIL, $2 = label, $3 = detail (optional).
record() {
  local status="$1" label="$2" detail="${3:-}"
  total=$((total + 1))
  if [[ "$status" == "PASS" ]]; then
    pass=$((pass + 1))
  fi
  printf '%-4s  %s%s\n' "$status" "$label" "${detail:+  ($detail)}"
}

# --- python validators ------------------------------------------------------
# Each reads the captured stderr file on stdin and exits 0 (valid) or nonzero
# (with a one-line reason on its own stderr).

# Usage: validate_json <file> <expected-kind>
PY_JSON='
import json, sys
expected = sys.argv[1]
data = sys.stdin.read()
lines = [ln for ln in data.splitlines() if ln.strip()]
if not lines:
    sys.stderr.write("no findings on stderr")
    sys.exit(1)
kinds = []
for i, ln in enumerate(lines, 1):
    try:
        obj = json.loads(ln)
    except Exception as e:
        sys.stderr.write("line %d is not valid JSON: %s" % (i, e))
        sys.exit(1)
    if not isinstance(obj, dict):
        sys.stderr.write("line %d is not a JSON object" % i)
        sys.exit(1)
    if obj.get("tool") != "redzone":
        sys.stderr.write("line %d missing tool==redzone" % i)
        sys.exit(1)
    kinds.append(obj.get("error"))
if expected not in kinds:
    sys.stderr.write("expected error %r not found (got %r)" % (expected, kinds))
    sys.exit(1)
'

# Usage: validate_sarif <file> <expected-kind>
PY_SARIF='
import json, sys
expected = sys.argv[1]
data = sys.stdin.read()
try:
    doc = json.loads(data)
except Exception as e:
    sys.stderr.write("stderr is not a single JSON document: %s" % e)
    sys.exit(1)
if doc.get("version") != "2.1.0":
    sys.stderr.write("version != 2.1.0 (got %r)" % doc.get("version"))
    sys.exit(1)
try:
    rule = doc["runs"][0]["results"][0]["ruleId"]
except Exception as e:
    sys.stderr.write("runs[0].results[0].ruleId not reachable: %s" % e)
    sys.exit(1)
if rule != expected:
    sys.stderr.write("ruleId %r != expected %r" % (rule, expected))
    sys.exit(1)
driver = doc["runs"][0].get("tool", {}).get("driver", {}).get("name")
if driver != "redzone":
    sys.stderr.write("tool.driver.name != redzone (got %r)" % driver)
    sys.exit(1)
'

# Usage: validate_empty <file>  -- assert no findings were emitted.
PY_EMPTY='
import sys
if sys.stdin.read().strip():
    sys.stderr.write("expected no findings, but stderr was nonempty")
    sys.exit(1)
'

# Build an example into the temp dir. Echoes the binary path on success.
build_example() {
  local name="$1" out="$TMP/$1"
  if ./scripts/redzone build "examples/${name}.c" -o "$out" >"$TMP/${name}.build.log" 2>&1; then
    printf '%s\n' "$out"
    return 0
  fi
  return 1
}

# Run the instrumented binary, capturing its findings (stderr) to a file while
# discarding its own stdout. A buggy example dies via SIGABRT; we run it inside
# an inner `bash -c` so that inner shell reaps the signal death and exits
# normally (128+signo), and we silence the inner shell's own stderr so bash's
# job-control "Abort trap: 6" notice never reaches our output. The findings
# themselves are written by the program straight to "$2".
#   $1 = binary path, $2 = stderr capture file, $3 = REDZONE_FORMAT value
run_capturing() {
  REDZONE_FORMAT="$3" bash -c '"$0" >/dev/null 2>"$1"' "$1" "$2" 2>/dev/null
}

# Run one buggy example and validate both json and sarif output.
# $1 = example basename (no .c), $2 = expected error-kind / ruleId
check_buggy() {
  local name="$1" kind="$2" bin
  if ! bin="$(build_example "$name")"; then
    record FAIL "$name build" "see $TMP/${name}.build.log"
    record FAIL "$name json" "skipped: build failed"
    record FAIL "$name sarif" "skipped: build failed"
    return
  fi

  # JSON: findings go to stderr; program's own stdout is discarded.
  run_capturing "$bin" "$TMP/${name}.json" json
  local detail
  if detail="$(python3 -c "$PY_JSON" "$kind" <"$TMP/${name}.json" 2>&1)"; then
    record PASS "$name json" "error=$kind"
  else
    record FAIL "$name json" "$detail"
  fi

  # SARIF.
  run_capturing "$bin" "$TMP/${name}.sarif" sarif
  if detail="$(python3 -c "$PY_SARIF" "$kind" <"$TMP/${name}.sarif" 2>&1)"; then
    record PASS "$name sarif" "ruleId=$kind"
  else
    record FAIL "$name sarif" "$detail"
  fi
}

# Run a clean example and assert it produces NO findings (exit 0, empty stderr).
check_clean() {
  local name="$1" bin
  if ! bin="$(build_example "$name")"; then
    record FAIL "$name build" "see $TMP/${name}.build.log"
    record FAIL "$name json-clean" "skipped: build failed"
    return
  fi

  run_capturing "$bin" "$TMP/${name}.json" json
  local code=$?
  local detail
  if [[ "$code" -ne 0 ]]; then
    record FAIL "$name json-clean" "expected exit 0, got $code"
    return
  fi
  if detail="$(python3 -c "$PY_EMPTY" <"$TMP/${name}.json" 2>&1)"; then
    record PASS "$name json-clean" "no findings, exit 0"
  else
    record FAIL "$name json-clean" "$detail"
  fi
}

echo "=== redzone machine-readable output tests ==="
echo

# Buggy examples: (example basename) -> (expected error-kind / ruleId)
check_buggy heap_overflow  heap-buffer-overflow
check_buggy use_after_free use-after-free
check_buggy stack_overflow stack-buffer-overflow
check_buggy double_free    double-free
check_buggy memory_leak    memory-leak

# Clean example: must emit no findings.
check_clean valid

echo
echo "${pass}/${total} passed"
[[ "$pass" -eq "$total" ]]
