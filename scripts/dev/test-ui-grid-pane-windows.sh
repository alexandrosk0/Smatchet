#!/usr/bin/env bash
# test-ui-grid-pane-windows.sh — bucket-E driver for multi-grid-tabs Slice 2
# (docs/plans/shipped/multi-grid-tabs.md items 14-16): dockable grid-pane
# new / focus-follow / side-by-side split / close smoke in
# tests/ui/grid_pane_windows.test.cpp.
#
# Mirrors test-ui-funcsize-grid-render.sh: injects the deterministic Jira
# fixture via SMATCHET_TEST_JIRA_BACKEND_FIXTURE so the panes render real
# tickets (SMAT-1/SMAT-2); without it the GridPanes test SKIPS.
#
# Exit codes:
#   0 — every test passed
#   1 — at least one test failed
#   2 — binary missing OR build had SMATCHET_BUILD_UI_TESTS=OFF OR fixture missing

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-ui-test-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
TEST_PORT="${SMATCHET_TEST_PORT:-58747}"
# imgui_test_engine filter — matches the GridPanes test group registered in
# tests/ui/grid_pane_windows.test.cpp.
FILTER="${UI_TEST_FILTER:-GridPanes}"
FIXTURE_DIR="${SMATCHET_FIXTURE_DIR:-tests/fixtures/jira_backend}"
FIXTURE="${SMATCHET_TEST_JIRA_BACKEND_FIXTURE:-$FIXTURE_DIR/basic-grid.json}"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone" >&2
    exit 2
fi

if [ ! -f "$FIXTURE" ]; then
    echo "FAIL: fixture not found: $FIXTURE" >&2
    exit 2
fi

# Isolated user-data dir so this run never writes to the developer's real profile.
TMPDIR_DATA="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_DATA"' EXIT

# Pre-seed the config: a fresh profile shows the first-launch Whisper setup
# banner (##WhisperSetupBanner), which floats OVER the grid window's header row
# and swallows the test's ItemClick on the "+" pane button / quick filter.
# Marking setup completed keeps the run isolated AND banner-free.
cat > "$TMPDIR_DATA/smatchet_config.json" <<'CFG'
{"read_only_mode": false, "whisper_setup_completed": true, "backend_has_been_reachable": true}
CFG

echo "[test-ui-grid-pane-windows] launching ephemeral Smatchet (port $TEST_PORT)..."
echo "  fixture: $FIXTURE"

RAW_OUTPUT="$(SMATCHET_USER_DATA="$TMPDIR_DATA" \
    SMATCHET_TEST_JIRA_BACKEND_FIXTURE="$FIXTURE" \
    "$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
    --mcp-port="$TEST_PORT" 2>&1 || true)"

echo "$RAW_OUTPUT" | tail -40

# Extract the JSON envelope (the `cmd` runner emits `{...}` as the last line).
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
