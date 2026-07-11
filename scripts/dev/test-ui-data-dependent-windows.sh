#!/usr/bin/env bash
# test-ui-data-dependent-windows.sh — bucket-E driver for the data-dependent
# UI window render-smoke tests (plan #12, batches 1 + 2 — all 8 windows).
# Exercises: Views Dashboard, Bulk Export, Plan Doc Viewer, Performance Window,
# Annotate Analysis (batch 1) + Attachment Preview, New Issue Draft row, Offline
# Queue panel (batch 2) — 8 tests in the DataDependentWindowsSmoke category.
#
# The New Issue Draft row test is FIXTURE-COUPLED (the grid table short-circuits
# without loaded tickets), so this driver injects the deterministic Jira fixture
# via SMATCHET_TEST_JIRA_BACKEND_FIXTURE when present — mirroring
# test-ui-funcsize-grid-render.sh. If the fixture file is absent the draft-row
# test SKIPS gracefully (logged, not failed); the other 7 still run.
#
# Exit codes:
#   0 — every test passed
#   1 — at least one test failed
#   2 — binary missing OR build had SMATCHET_BUILD_UI_TESTS=OFF

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-ui-test-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
TEST_PORT="${SMATCHET_TEST_PORT:-58735}"
# Filter: substring-match against "DataDependentWindowsSmoke" — picks up all
# 8 tests registered in data_dependent_windows_smoke.test.cpp.
# imgui_te_engine filter language: substring (not glob); comma-separated OR.
FILTER="${UI_TEST_FILTER:-DataDependentWindowsSmoke}"
FIXTURE_DIR="${SMATCHET_FIXTURE_DIR:-tests/fixtures/jira_backend}"
FIXTURE="${SMATCHET_TEST_JIRA_BACKEND_FIXTURE:-$FIXTURE_DIR/basic-grid.json}"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found." >&2
    echo "Build: cmake --preset ninja-test-msvc -DFETCHCONTENT_BASE_DIR=C:/Development/Smatchet/.fetchcontent-src" >&2
    echo "       cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone" >&2
    exit 2
fi

# Isolated user-data dir so this run never writes to the developer's real profile
# (the Offline Queue test enqueues + deletes a synthetic SQLite create row).
TMPDIR_DATA="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_DATA"' EXIT

echo "[test-ui-data-dependent-windows] launching ephemeral Smatchet (port $TEST_PORT)..."
if [ -f "$FIXTURE" ]; then
    echo "  fixture: $FIXTURE (New Issue Draft row test enabled)"
    RAW_OUTPUT="$(SMATCHET_USER_DATA="$TMPDIR_DATA" \
        SMATCHET_TEST_JIRA_BACKEND_FIXTURE="$FIXTURE" \
        "$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
        --mcp-port="$TEST_PORT" 2>&1 || true)"
else
    echo "  fixture: NONE at $FIXTURE — New Issue Draft row test will SKIP"
    RAW_OUTPUT="$(SMATCHET_USER_DATA="$TMPDIR_DATA" \
        "$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
        --mcp-port="$TEST_PORT" 2>&1 || true)"
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
