#!/usr/bin/env bash
# test-ui-jira-deterministic-backend.sh — bucket-E driver for the deterministic
# Jira backend UI tests (Slice 4 of deterministic-jira-test-backend.md).
# Sets an isolated SMATCHET_USER_DATA dir, injects the basic-grid fixture via
# SMATCHET_TEST_JIRA_BACKEND_FIXTURE, invokes `ui_test.run`, and parses the
# JSON envelope.
#
# Exit codes:
#   0 — every test passed
#   1 — at least one test failed
#   2 — binary missing OR build had SMATCHET_BUILD_UI_TESTS=OFF

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-ui-test-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
TEST_PORT="${SMATCHET_TEST_PORT:-58736}"
# imgui_test_engine filter — matches the JiraDeterministic* test group registered
# in tests/ui/jira_deterministic_backend.test.cpp.
FILTER="${UI_TEST_FILTER:-JiraDeterministic}"
FIXTURE_DIR="${SMATCHET_FIXTURE_DIR:-tests/fixtures/jira_backend}"
FIXTURE="${SMATCHET_TEST_JIRA_BACKEND_FIXTURE:-$FIXTURE_DIR/basic-grid.json}"
# Per-test verbose-log dump. When SMATCHET_UI_TEST_OUTLOG is set, pass it through
# to `ui_test.run --outLog=` so every test's Output.Log lands on disk; we `cat` it
# on failure for diagnosis (the --spawn parent only prints pass/fail counts).
OUTLOG="${SMATCHET_UI_TEST_OUTLOG:-}"

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

echo "[test-ui-jira-deterministic-backend] launching ephemeral Smatchet (port $TEST_PORT)..."
echo "  fixture: $FIXTURE"

OUTLOG_ARG=()
OUTLOG_RELATIVE=""
if [ -n "$OUTLOG" ]; then
    # ui_test.run's --outLog is confined to a bare relative filename under
    # <userData>/ui-tests/ (PathConfinement.h, shipped in the SECURITY_AUDIT.md
    # sweep, PR #1566) — the caller-supplied $OUTLOG (an absolute path, e.g. CI's
    # $GITHUB_WORKSPACE/build/tmp/...) is rejected outright since #1566 landed.
    # Pass just the basename and copy the confined result back to $OUTLOG below,
    # before $TMPDIR_DATA is removed — this keeps the external contract (a file at
    # $OUTLOG afterward) unchanged for every caller, including this repo's own CI
    # workflow's cat/upload-artifact steps.
    OUTLOG_RELATIVE="$(basename "$OUTLOG")"
    OUTLOG_ARG=(--outLog="$OUTLOG_RELATIVE")
    echo "  outLog: $OUTLOG_RELATIVE (confined under \$SMATCHET_USER_DATA/ui-tests/; copied to $OUTLOG after the run)"
fi

RAW_OUTPUT="$(SMATCHET_USER_DATA="$TMPDIR_DATA" \
    SMATCHET_TEST_JIRA_BACKEND_FIXTURE="$FIXTURE" \
    "$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
    "${OUTLOG_ARG[@]}" \
    --mcp-port="$TEST_PORT" 2>&1 || true)"

echo "$RAW_OUTPUT" | tail -40

# Copy the confined outLog out of the ephemeral $TMPDIR_DATA (removed by the EXIT
# trap above) to the caller-requested $OUTLOG, before anything below reads $OUTLOG.
if [ -n "$OUTLOG_RELATIVE" ]; then
    CONFINED_OUTLOG="$TMPDIR_DATA/ui-tests/$OUTLOG_RELATIVE"
    if [ -f "$CONFINED_OUTLOG" ]; then
        mkdir -p "$(dirname "$OUTLOG")"
        cp "$CONFINED_OUTLOG" "$OUTLOG"
    fi
fi

# Best-effort: on any non-clean run, surface the per-test verbose log if present.
cat_outlog_on_failure() {
    if [ -n "$OUTLOG" ] && [ -f "$OUTLOG" ]; then
        echo
        echo "===== ui_test per-test verbose log ($OUTLOG) ====="
        cat "$OUTLOG"
        echo "===== end per-test verbose log ====="
    fi
}

# Extract the JSON envelope (the `cmd` runner emits `{...}` as the last line).
JSON_LINE="$(echo "$RAW_OUTPUT" | grep -oE '\{.*\}' | tail -1 || true)"
if [ -z "$JSON_LINE" ]; then
    echo "FAIL: could not extract JSON envelope from ui_test.run output" >&2
    cat_outlog_on_failure
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
    cat_outlog_on_failure
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
    cat_outlog_on_failure
    exit 1
fi
exit 0
