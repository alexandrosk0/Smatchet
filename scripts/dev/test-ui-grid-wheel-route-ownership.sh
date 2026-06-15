#!/usr/bin/env bash
# Bucket-E driver for the active-project grid wheel-route ownership regression.

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-ui-test-msvc/Smatchet.exe}"
TEST_PORT="${SMATCHET_TEST_PORT:-58741}"
FILTER="${UI_TEST_FILTER:-WheelRouteOwnership}"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone" >&2
    exit 2
fi

echo "[test-ui-grid-wheel-route-ownership] launching ephemeral Smatchet (port $TEST_PORT)..."
RAW_OUTPUT="$("$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
    --mcp-port="$TEST_PORT" 2>&1 || true)"

echo "$RAW_OUTPUT" | tail -40

JSON_LINE="$(printf '%s\n' "$RAW_OUTPUT" | tr -d '\r' | awk '/^\{.*\}$/{line=$0} END{print line}')"
if [ -z "$JSON_LINE" ]; then
    echo "FAIL: could not extract JSON envelope from ui_test.run output" >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

PASSED="$(printf '%s\n' "$JSON_LINE" | sed -n 's/.*"passed":\([0-9][0-9]*\).*/\1/p')"
FAILED="$(printf '%s\n' "$JSON_LINE" | sed -n 's/.*"failed":\([0-9][0-9]*\).*/\1/p')"
LOG="$(printf '%s\n' "$JSON_LINE" | sed -n 's/.*"log":"\([^"]*\)".*/\1/p')"
PASSED="${PASSED:-?}"
FAILED="${FAILED:-?}"
LOG="${LOG:-?}"

echo
echo "Result: passed=$PASSED failed=$FAILED log=$LOG"

if [ "$LOG" = "build had SMATCHET_BUILD_UI_TESTS=OFF" ]; then
    echo "FAIL: build had SMATCHET_BUILD_UI_TESTS=OFF - rebuild with ninja-ui-test-msvc." >&2
    echo "Passed: 0  Failed: 0"
    exit 2
fi

if [ "$PASSED" = "?" ] || [ "$FAILED" = "?" ]; then
    echo "Passed: 0  Failed: 1"
    exit 1
fi

if [ "$PASSED" = "0" ] && [ "$FAILED" = "0" ]; then
    echo "FAIL: 0 tests matched filter '$FILTER'" >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

echo "Passed: $PASSED  Failed: $FAILED"
if [ "$FAILED" != "0" ]; then
    exit 1
fi
exit 0
