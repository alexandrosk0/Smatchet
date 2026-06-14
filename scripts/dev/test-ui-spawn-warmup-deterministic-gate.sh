#!/usr/bin/env bash
# test-ui-spawn-warmup-deterministic-gate.sh — bucket-E driver for the
# --spawn warmup gate added by slice 9 of autonomous-debugging-no-creds.
# Closes infra.md P2 line 16 (intermittent --spawn flake).

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-ui-test-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
TEST_PORT="${SMATCHET_TEST_PORT:-58805}"
FILTER="${UI_TEST_FILTER:-SpawnWarmup}"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone" >&2
    exit 2
fi

echo "[test-ui-spawn-warmup-deterministic-gate] launching ephemeral Smatchet (port $TEST_PORT)..."
RAW_OUTPUT="$("$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
    --mcp-port="$TEST_PORT" 2>&1 || true)"

echo "$RAW_OUTPUT" | tail -40

JSON_LINE="$(printf '%s\n' "$RAW_OUTPUT" | "$PY" -c '
import json, sys
last = ""
for line in sys.stdin:
    s = line.strip()
    if not s.startswith("{"):
        continue
    try:
        obj = json.loads(s)
    except Exception:
        continue
    d = obj.get("data", {}) if isinstance(obj, dict) else {}
    if "passed" in d and "failed" in d:
        last = s
print(last)
' || true)"
if [ -z "$JSON_LINE" ]; then
    echo "FAIL: could not extract JSON envelope from ui_test.run output" >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

PASSED="$(echo "$JSON_LINE" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('passed','?'))" 2>/dev/null || echo "?")"
FAILED="$(echo "$JSON_LINE" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('failed','?'))" 2>/dev/null || echo "?")"
LOG="$(echo "$JSON_LINE" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('log','?'))" 2>/dev/null || echo "?")"

echo
echo "Result: passed=$PASSED failed=$FAILED log=$LOG"

if [ "$LOG" = "build had SMATCHET_BUILD_UI_TESTS=OFF" ]; then
    echo "FAIL: build had SMATCHET_BUILD_UI_TESTS=OFF — rebuild with ninja-ui-test-msvc." >&2
    echo "Passed: 0  Failed: 0"
    exit 2
fi

if [ "$PASSED" = "?" ] || [ "$FAILED" = "?" ]; then
    echo "Passed: 0  Failed: 1"
    exit 1
fi

# Zero-match guard (fail-closed): a renamed/dropped registration or a filter
# typo makes ui_test.run match 0 tests and report passed=0 failed=0. Without
# this a 0-test run exits green with ZERO coverage — the fail-open class the
# merge-gates poller was hardened against. A driver that runs 0 tests is broken,
# not passing. Mirrors test-ui-duration-inline-edit.sh.
if [ "$PASSED" = "0" ] && [ "$FAILED" = "0" ]; then
    echo "FAIL: ui_test.run matched 0 tests for filter '$FILTER' — registration or filter problem." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

echo "Passed: $PASSED  Failed: $FAILED"
if [ "$FAILED" != "0" ]; then
    exit 1
fi
exit 0
