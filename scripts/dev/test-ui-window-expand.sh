#!/usr/bin/env bash
# test-ui-window-expand.sh — bucket-E driver for the per-window Expand /
# Minimize control (window-expand-button.md). Invokes the WindowExpand tests
# against an ephemeral spawn, parses the JSON envelope, reports Passed / Failed.
#
# The tests boot the real app, open secondary windows (Log / Backend Audit),
# expand them over the viewport work area, minimize them back into the dock
# node they left, hand the fullscreen slot between two windows, click both faces
# of the real control (checking the docked one is glued to the tab bar's right
# edge), and check the closed-while-expanded self-heal plus the main grid pane's
# expand-grows-a-title-bar path. That geometry coverage is what keeps this UI
# feature out of the visual-validation user-pause. See tests/ui/window_expand.test.cpp.
#
# No backend fixture required — every window under test renders with zero
# tracker state.
#
# Exit codes:
#   0 — every test passed
#   1 — at least one test failed
#   2 — binary missing OR build had SMATCHET_BUILD_UI_TESTS=OFF

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-ui-test-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
TEST_PORT="${SMATCHET_TEST_PORT:-58816}"
# imgui_test_engine's filter is substring-match (NOT a glob); "WindowExpand" is
# the test category, so this drives every case in one run.
FILTER="${UI_TEST_FILTER:-WindowExpand}"

# Boot against a THROWAWAY user-data dir so the dock layout is the app's built-in
# default every run. Without this the app resolves %LOCALAPPDATA%\Smatchet and
# loads whatever imgui.ini the developer's own session last wrote — windows the
# test never opened end up floating over the dock tab bar, and ItemClick on the
# docked toggle cannot hover it ("Failed to move window ...! While trying to make
# space to click at ..."). That is machine state, not a product defect, and it
# made this suite pass or fail depending on whose desktop it ran on.
# The vars are the ones ConfigManager::GetPlatformSharedUserDataDirectory reads
# (ConfigManager_PathUtils.cpp): LOCALAPPDATA/APPDATA on Windows, XDG_CONFIG_HOME
# elsewhere. Exported for the child only — the caller's environment is untouched.
# SMATCHET_UI_TEST_HOME pins the dir instead of minting one, and suppresses the
# cleanup — the app's own log lives under it, and a failing run is undebuggable
# once the throwaway dir is gone.
if [ -n "${SMATCHET_UI_TEST_HOME:-}" ]; then
    UI_TEST_HOME="$SMATCHET_UI_TEST_HOME"
    mkdir -p "$UI_TEST_HOME"
    echo "[test-ui-window-expand] user-data dir pinned to $UI_TEST_HOME (kept on exit)"
    echo "[test-ui-window-expand] layout state persists across pinned runs — a red here may be"
    echo "[test-ui-window-expand] a stale imgui.ini; re-run unpinned to confirm before believing it."
else
    UI_TEST_HOME="$(mktemp -d 2>/dev/null || echo "${TMPDIR:-/tmp}/smatchet-ui-window-expand-$$")"
    mkdir -p "$UI_TEST_HOME"
    cleanup_ui_test_home() { rm -rf "$UI_TEST_HOME"; }
    trap cleanup_ui_test_home EXIT
fi
export LOCALAPPDATA="$UI_TEST_HOME" APPDATA="$UI_TEST_HOME" XDG_CONFIG_HOME="$UI_TEST_HOME"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone" >&2
    exit 2
fi

echo "[test-ui-window-expand] launching ephemeral Smatchet (port $TEST_PORT)..."
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
