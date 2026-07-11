#!/usr/bin/env bash
# test-ui-callstack-tooltip-hover.sh — bucket-E driver for the callstack-cell
# tooltip hover regression gate (SmatchetFieldRender.cpp::RenderClippedFieldText
# BeginGroup/EndGroup wrap). Covers all four variants:
#   - FirstToken / MiddleToken — replica-flag positive cases (the BeginGroup fix)
#   - MiddleToken_NoGroupWrap — methodology self-check (regression shape)
#   - Production_ContentIdentity — production-driven gate using
#     BucketE::TooltipContentMatches (plan b8-bucket-e-coverage L3; tooling #101)
# Invokes `ui_test.run` against an ephemeral --spawn instance, parses the JSON
# envelope, reports Passed / Failed.
#
# Exit codes:
#   0 — every test passed
#   1 — at least one test failed
#   2 — binary missing OR build had SMATCHET_BUILD_UI_TESTS=OFF

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-ui-test-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
TEST_PORT="${SMATCHET_TEST_PORT:-58743}"
# Matches all four CallstackTooltipHover_* variants registered in
# tests/ui/callstack_tooltip_hover.test.cpp.
FILTER="${UI_TEST_FILTER:-CallstackTooltipHover}"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone" >&2
    exit 2
fi

echo "[test-ui-callstack-tooltip-hover] launching ephemeral Smatchet (port $TEST_PORT)..."
# Hard timeout so a hung spawn / wedged test run can't block forever and stall CI
# (CodeRabbit #1364). Override via UI_TEST_TIMEOUT. `timeout` exits 124 on expiry.
RUN_TIMEOUT="${UI_TEST_TIMEOUT:-240}"
# `timeout` (coreutils) ships in git-bash + the Linux CI shells, but guard anyway so a
# shell that lacks it degrades to no-timeout instead of erroring out (CodeRabbit #1364).
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_PREFIX=(timeout "$RUN_TIMEOUT")
else
    echo "[test-ui-callstack-tooltip-hover] WARN: 'timeout' not found; running without a hard timeout." >&2
    TIMEOUT_PREFIX=()
fi
set +e
RAW_OUTPUT="$("${TIMEOUT_PREFIX[@]}" "$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
    --mcp-port="$TEST_PORT" 2>&1)"
RUN_RC=$?
set -e
if [ "$RUN_RC" -eq 124 ]; then
    echo "$RAW_OUTPUT" | tail -40
    echo "FAIL: ui_test.run exceeded ${RUN_TIMEOUT}s hard timeout (hung spawn?) — aborted." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

echo "$RAW_OUTPUT" | tail -40

JSON_LINE="$(echo "$RAW_OUTPUT" | grep -oE '\{.*\}' | tail -1 || true)"
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

echo "Passed: $PASSED  Failed: $FAILED"
# Zero-run floor (fail-open shape Z): a run that produces ZERO results leaves
# PASSED=FAILED=0 and would exit green - a vanished/unparsed suite passing.
if [ "$PASSED" -eq 0 ] && [ "$FAILED" -eq 0 ]; then
    echo "$(basename "$0" .sh): FAIL - the test run produced ZERO results (vanished / unparsed)." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi
if [ "$FAILED" != "0" ]; then
    exit 1
fi
exit 0
