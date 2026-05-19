#!/usr/bin/env bash
# test-ui-agent-proposals-handoff-button.sh — bucket-E driver for the H9
# `[Start handoff]` button on the T6 Agent proposals panel (four variants —
# CommentAdd_NoHandoffButton / LabelAdd_NoHandoffButton /
# ImplementIssue_ButtonVisible / ImplementIssue_ClickStartsHandoff).
# Invokes `ui_test.run` against an ephemeral spawn, parses the JSON envelope,
# and reports Passed / Failed.
#
# Exit codes:
#   0 — every test passed
#   1 — at least one test failed
#   2 — binary missing OR build had SMATCHET_BUILD_UI_TESTS=OFF

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-ui-test-msys2/Smatchet.exe}"
PY="${PYTHON:-python}"
TEST_PORT="${SMATCHET_TEST_PORT:-58743}"
# Substring match — "AgentProposalsHandoffButton" matches all four registered variants.
FILTER="${UI_TEST_FILTER:-AgentProposalsHandoffButton}"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-ui-test-msys2 --target SmatchetStandalone" >&2
    exit 2
fi

echo "[test-ui-agent-proposals-handoff-button] launching ephemeral Smatchet (port $TEST_PORT)..."
RAW_OUTPUT="$("$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
    --port="$TEST_PORT" 2>&1 || true)"

echo "$RAW_OUTPUT" | tail -40

# Greedy `grep -oE '\{.*\}'` is brittle when stdout interleaves log lines and
# the JSON envelope — it can capture a log fragment that happens to contain
# braces, or split a pretty-printed object across lines. Prefer parsing line
# by line and keeping the last line that parses cleanly as JSON.
JSON_LINE=""
if command -v "$PY" >/dev/null 2>&1; then
    JSON_LINE="$(printf '%s\n' "$RAW_OUTPUT" | "$PY" -c "
import json, sys
last = None
for line in sys.stdin:
    s = line.strip()
    if not s or s[0] != '{':
        continue
    try:
        json.loads(s)
        last = s
    except json.JSONDecodeError:
        continue
if last is not None:
    sys.stdout.write(last)
" 2>/dev/null || true)"
fi

# Fallback for environments without Python on PATH: grep for the last line
# that both starts with `{` and ends with `}`. Brittler than the Python pass
# but tolerates a single-line envelope which is what Smatchet emits today.
if [ -z "$JSON_LINE" ]; then
    JSON_LINE="$(echo "$RAW_OUTPUT" | grep -E '^\s*\{.*\}\s*$' | tail -1 || true)"
fi

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
# Same fail-on-zero gate as test-ui-agent-proposals.sh — a missing or renamed
# filter must surface as a failure, not a silent pass.
if [ "$PASSED" = "0" ] && [ "$FAILED" = "0" ]; then
    echo "FAIL: no tests ran (filter='$FILTER' matched nothing). Likely cause: bucket-E test renamed or excluded." >&2
    exit 1
fi
if [ "$FAILED" != "0" ]; then
    exit 1
fi
exit 0
