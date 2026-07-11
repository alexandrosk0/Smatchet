#!/usr/bin/env bash
# test-lua-error-log.sh — automated validation of the persistent Lua error log feature.
# Exercises debug.lua_log_test (BuiltinCommands.cpp) against an ephemeral --spawn instance.
#
# Validates:
#   1. log_info() invokes the log sink — Lua → LuaLogInfoBind → SnapshotLogSinks → capture.
#   2. error() invokes BOTH the log sink (with "[ERROR]" prefix) AND the dedicated error sink.
#   3. error() sets scriptingWindowOpenRequested_ so the Scripting window auto-opens in UI.
#   4. log_info() does NOT set scriptingWindowOpenRequested_ (only errors do).
#
# Exit codes:
#   0 — all assertions passed
#   1 — a regression was detected (assertion failed)
#   2 — the binary or build is missing
#
# Usage: bash scripts/dev/test-lua-error-log.sh

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-iter-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
# Use a non-default MCP port so the CLI cannot short-circuit to an already-running Smatchet
# instance (which would still be on the old binary if the user is testing in the UI). With
# the port unreachable, --spawn always launches a fresh ephemeral child against the rebuilt
# exe — which is the one we want to validate.
TEST_PORT="${SMATCHET_TEST_PORT:-58731}"

if [ ! -x "$EXE" ] && [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-iter-msvc --target SmatchetStandalone" >&2
    exit 2
fi

PASSED=0
FAILED=0

# Extract a JSON field from the CLI's text-wrapped output. The Python one-liner finds the
# first '{' through the matching '}' and parses it as JSON.
extract() {
    local field="$1"
    "$PY" -c "import sys,json,re; t=sys.stdin.read(); m=re.search(r'\{.*\}',t,re.S); d=json.loads(m.group(0)); print(json.dumps(d.get('data',{}).get('$field','MISSING')))"
}

assert_eq() {
    local label="$1" actual="$2" expected="$3"
    if [ "$actual" = "$expected" ]; then
        echo "  PASS  $label  (got=$actual)"
        PASSED=$((PASSED + 1))
    else
        echo "  FAIL  $label  expected=$expected got=$actual"
        FAILED=$((FAILED + 1))
    fi
}

assert_contains() {
    local label="$1" haystack="$2" needle="$3"
    if echo "$haystack" | grep -q "$needle"; then
        echo "  PASS  $label  (matched=$needle)"
        PASSED=$((PASSED + 1))
    else
        echo "  FAIL  $label  needle=$needle haystack=$haystack"
        FAILED=$((FAILED + 1))
    fi
}

run_test() {
    local code="$1"
    "$EXE" cmd debug.lua_log_test --code="$code" --mcp-port="$TEST_PORT" --spawn --yes 2>&1
}

echo
echo "=== Test 1: log_info() reaches the log sink ==="
OUT=$(run_test "log_info('hello-from-test')")
assert_eq      "ok_snippet"                "$(echo "$OUT" | extract ok_snippet)"                "true"
assert_contains "log_lines contains hello"  "$(echo "$OUT" | extract log_lines)"                 "hello-from-test"
assert_eq      "window_requested off"      "$(echo "$OUT" | extract window_requested)"           "false"
assert_eq      "err_lines empty"           "$(echo "$OUT" | extract err_lines)"                  "[]"

echo
echo "=== Test 2: error() reaches BOTH log sink (with [ERROR]) and error sink ==="
OUT=$(run_test "error('boom-from-test')")
assert_eq      "ok_snippet false on throw"  "$(echo "$OUT" | extract ok_snippet)"               "false"
assert_contains "snippet_error has boom"    "$(echo "$OUT" | extract snippet_error)"            "boom-from-test"
assert_contains "log_lines has [ERROR]"     "$(echo "$OUT" | extract log_lines)"                "\\[ERROR\\]"
assert_contains "err_lines has boom"        "$(echo "$OUT" | extract err_lines)"                "boom-from-test"
assert_eq      "window_requested on"       "$(echo "$OUT" | extract window_requested)"          "true"

echo
echo "=== Test 3: log_info()+error() sequence — both lines captured ==="
OUT=$(run_test "log_info('line-A'); error('line-B')")
assert_contains "log_lines has line-A"     "$(echo "$OUT" | extract log_lines)"                 "line-A"
assert_contains "log_lines has [ERROR]"    "$(echo "$OUT" | extract log_lines)"                 "\\[ERROR\\]"
assert_contains "err_lines has line-B"     "$(echo "$OUT" | extract err_lines)"                 "line-B"
assert_eq      "window_requested on"       "$(echo "$OUT" | extract window_requested)"          "true"

echo
echo "=== Test 4: parse error on bad syntax also fires error sink ==="
OUT=$(run_test "this is not valid lua %$#")
assert_eq      "ok_snippet false on parse" "$(echo "$OUT" | extract ok_snippet)"                "false"
assert_eq      "window_requested on"       "$(echo "$OUT" | extract window_requested)"          "true"

echo
echo "============================="
echo "Passed: $PASSED  Failed: $FAILED"
echo "============================="

# Zero-run floor (fail-open shape Z): a run that produces ZERO results leaves
# PASSED=FAILED=0 and would exit green - a vanished/unparsed suite passing.
if [ "$PASSED" -eq 0 ] && [ "$FAILED" -eq 0 ]; then
    echo "$(basename "$0" .sh): FAIL - the test run produced ZERO results (vanished / unparsed)." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi
if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
exit 0
