#!/usr/bin/env bash
# test-ui-grid-spreadsheet.sh — bucket-E driver for the grid's keyboard cell cursor
# (tests/ui/grid_spreadsheet_keys.test.cpp, docs/plans/shipped/input-ux-w3-grid-spreadsheet.md, slice S1). Runs
# `ui_test.run --name=GridSpreadsheet` against an ephemeral spawn on the 60-issue grid-spreadsheet.json fixture and
# parses the JSON envelope.
#
# Covers through real clicks and key presses: the ID-cell click that makes the active cell, one arrow press = one cell,
# Home / End / Ctrl+Home, Shift+arrow range growth, the active cell scrolling into view below the frozen header, a
# drag between cells selecting a range with no editor open, and the active cell + range following their rows (by id)
# through a narrowing search.
#
# Spawn-warmup retry: the headless `--spawn` runner intermittently launches the ephemeral app "cold" and the whole
# queued test set fails 0-of-N (spawn_warmup_deterministic_gate.test.cpp). Retry ONLY on that wipeout signal
# (passed==0 && failed==tested), never on a partial failure.
#
# Exit codes:
#   0 — every test passed
#   1 — at least one test failed (genuine)
#   2 — binary or fixture missing OR build had SMATCHET_BUILD_UI_TESTS=OFF

set -euo pipefail

# Shared bucket-E preamble (exe staleness guard, throwaway profile, wedge-proof capture).
# shellcheck source=scripts/dev/lib/ui-test-driver.sh
. "$(dirname "$0")/lib/ui-test-driver.sh"

EXE="${SMATCHET_EXE:-build/ninja-ui-test-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
TEST_PORT="${SMATCHET_TEST_PORT:-58780}"
FILTER="${UI_TEST_FILTER:-GridSpreadsheet}"
MAX_ATTEMPTS="${UI_TEST_WARMUP_RETRIES:-8}"
# This lane owns its fixture: the CI loop exports basic-grid.json for the other drivers, and the test needs 60 rows.
FIXTURE="${SMATCHET_FIXTURE_DIR:-tests/fixtures/jira_backend}/grid-spreadsheet.json"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone" >&2
    exit 2
fi

if [ ! -f "$FIXTURE" ]; then
    echo "FAIL: fixture not found: $FIXTURE" >&2
    exit 2
fi

ui_test_require_fresh_exe "$EXE" || exit 2

# A loopback token requirement from the caller's shell would turn the CLI's own calls into 401s.
unset SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK

# Seeded profile: a fresh one shows the Whisper setup banner over the grid and starts read-only.
ui_test_isolate_home --seed

# Native Windows path for the exe (the repo-relative path works as is; absolute it so the spawn child cannot miss it).
FIXTURE="$(cd "$(dirname "$FIXTURE")" && pwd -W 2>/dev/null || pwd)/$(basename "$FIXTURE")"

field() {
    # field <json> <key> — extract data.<key>, default "?".
    echo "$1" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('$2','?'))" 2>/dev/null || echo "?"
}

PASSED="?"; FAILED="?"; TESTED="?"; LOG="?"
attempt=1
while [ "$attempt" -le "$MAX_ATTEMPTS" ]; do
    PORT=$((TEST_PORT + attempt - 1))
    echo "[test-ui-grid-spreadsheet] attempt $attempt/$MAX_ATTEMPTS — launching ephemeral Smatchet (port $PORT)..."
    echo "  fixture: $FIXTURE"
    RAW_OUTPUT="$(SMATCHET_TEST_JIRA_BACKEND_FIXTURE="$FIXTURE" \
        "$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
        --mcp-port="$PORT" ${UI_TEST_OUTLOG:+--outLog="$UI_TEST_OUTLOG"} 2>&1 || true)"
    echo "$RAW_OUTPUT" | tail -12

    JSON_LINE="$(echo "$RAW_OUTPUT" | grep -oE '\{.*\}' | tail -1 || true)"
    if [ -z "$JSON_LINE" ]; then
        echo "  (no JSON envelope this attempt)"
        attempt=$((attempt + 1)); continue
    fi

    PASSED="$(field "$JSON_LINE" passed)"
    FAILED="$(field "$JSON_LINE" failed)"
    TESTED="$(field "$JSON_LINE" tested)"
    LOG="$(field "$JSON_LINE" log)"

    if [ "$LOG" = "build had SMATCHET_BUILD_UI_TESTS=OFF" ]; then
        echo "FAIL: build had SMATCHET_BUILD_UI_TESTS=OFF — rebuild with ninja-ui-test-msvc." >&2
        echo "Passed: 0  Failed: 0"
        exit 2
    fi

    # Whole-suite wipeout (passed==0 && failed==tested && tested>0) == cold-spawn warmup miss → retry.
    if [ "$PASSED" = "0" ] && [ "$FAILED" = "$TESTED" ] && [ "$TESTED" != "0" ] && [ "$TESTED" != "?" ]; then
        echo "  cold-spawn wipeout (0/$TESTED) — retrying warmup..."
        attempt=$((attempt + 1)); continue
    fi
    break
done

echo
echo "Result: tested=$TESTED passed=$PASSED failed=$FAILED log=$LOG"

if [ "$PASSED" = "?" ] || [ "$FAILED" = "?" ]; then
    echo "FAIL: could not extract pass/fail counts after $MAX_ATTEMPTS attempts (spawn-warmup flake)." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

# Zero-run floor: a run that produces ZERO results must not exit green (filter typo / dropped registration).
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
