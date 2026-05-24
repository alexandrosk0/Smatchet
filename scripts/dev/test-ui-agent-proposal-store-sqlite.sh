#!/usr/bin/env bash
# test-ui-agent-proposal-store-sqlite.sh — bucket-E driver for the
# AgentProposalStore SQLite-backed lifecycle coverage added by slice 9 of
# autonomous-debugging-no-creds. Closes the bucket-E SQLite coverage gap
# in test.md P2 (lane sub-rig framing re-disposed: SqliteMemFixture covers it).

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-ui-test-msys2/Smatchet.exe}"
PY="${PYTHON:-python}"
TEST_PORT="${SMATCHET_TEST_PORT:-58806}"
FILTER="${UI_TEST_FILTER:-AgentProposalStore}"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-ui-test-msys2 --target SmatchetStandalone" >&2
    exit 2
fi

echo "[test-ui-agent-proposal-store-sqlite] launching ephemeral Smatchet (port $TEST_PORT)..."
RAW_OUTPUT="$("$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
    --port="$TEST_PORT" 2>&1 || true)"

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
    echo "FAIL: build had SMATCHET_BUILD_UI_TESTS=OFF — rebuild with ninja-ui-test-msys2." >&2
    echo "Passed: 0  Failed: 0"
    exit 2
fi

if [ "$PASSED" = "?" ] || [ "$FAILED" = "?" ]; then
    echo "Passed: 0  Failed: 1"
    exit 1
fi

echo "Passed: $PASSED  Failed: $FAILED"
if [ "$FAILED" != "0" ]; then
    exit 1
fi
exit 0
