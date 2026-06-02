#!/usr/bin/env bash
# test-ui-funcsize-window-render-smoke.sh — Phase-0 pilot driver for the
# full-function-size-compliance program. Invokes the FuncSizeWindowRender
# bucket-E tests against an ephemeral spawn, parses the JSON envelope, and
# reports Passed / Failed.
#
# These tests boot the real app, open each target top-level window so its REAL
# draw function ticks every frame, and assert the window renders (Begin/End +
# PushID/PopID balanced — the ImGui Test Engine traps any imbalance) with an
# expected toolbar widget present. This is the structural coverage that lets
# the Phase-5 window-draw decompositions ship hands-off (no visual-validation
# user-pause). See tests/ui/funcsize_window_render_smoke.test.cpp.
#
# Exit codes:
#   0 — every test passed
#   1 — at least one test failed
#   2 — binary missing OR build had SMATCHET_BUILD_UI_TESTS=OFF

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-ui-test-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
TEST_PORT="${SMATCHET_TEST_PORT:-58765}"
# imgui_test_engine's filter is substring-match (NOT a glob). "FuncSizeWindowRender"
# is the shared test category, matching all three window-render variants
# registered in tests/ui/funcsize_window_render_smoke.test.cpp.
FILTER="${UI_TEST_FILTER:-FuncSizeWindowRender}"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone" >&2
    exit 2
fi

echo "[test-ui-funcsize-window-render-smoke] launching ephemeral Smatchet (port $TEST_PORT)..."
RAW_OUTPUT="$("$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
    --mcp-port="$TEST_PORT" 2>&1 || true)"

echo "$RAW_OUTPUT" | tail -40

# Find the JSON envelope (the `cmd` runner prints `{...}` to stdout last).
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
if [ "$FAILED" != "0" ]; then
    exit 1
fi
exit 0
