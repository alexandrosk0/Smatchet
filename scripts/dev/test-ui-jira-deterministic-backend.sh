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
# Per-test verbose-log dump. When SMATCHET_UI_TEST_OUTLOG is set it names the
# CI-visible destination for every test's Output.Log; we `cat` it on failure for
# diagnosis (the --spawn parent only prints pass/fail counts). NOTE: ui_test.run
# confines --outLog under <userData>/ui-tests/ and REJECTS absolute paths
# (Source/Core/include/Commands/PathConfinement.h — MCP-reachable arbitrary-write
# hardening), so we hand the handler a bare basename and copy the confined file
# back out to this destination after the run.
OUTLOG_DEST="${SMATCHET_UI_TEST_OUTLOG:-}"

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
OUTLOG_CONFINED=""
if [ -n "$OUTLOG_DEST" ]; then
    # Bare basename → the handler resolves it under <userData>/ui-tests/. An
    # absolute value here would be rejected by PathConfinement and the scenario
    # would fail to start.
    OUTLOG_NAME="$(basename "$OUTLOG_DEST")"
    OUTLOG_ARG=(--outLog="$OUTLOG_NAME")
    OUTLOG_CONFINED="$TMPDIR_DATA/ui-tests/$OUTLOG_NAME"
    echo "  outLog: $OUTLOG_NAME (confined under the isolated user-data dir)"
fi

RAW_OUTPUT="$(SMATCHET_USER_DATA="$TMPDIR_DATA" \
    SMATCHET_TEST_JIRA_BACKEND_FIXTURE="$FIXTURE" \
    "$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
    "${OUTLOG_ARG[@]}" \
    --mcp-port="$TEST_PORT" 2>&1 || true)"

echo "$RAW_OUTPUT" | tail -40

# Copy the confined per-test log out to the CI-visible destination before the
# TMPDIR_DATA trap wipes it. Best-effort: the log is a diagnostic aid, never a
# gate, so a missing/unresolved source never fails the run.
if [ -n "$OUTLOG_DEST" ] && [ -f "$OUTLOG_CONFINED" ]; then
    mkdir -p "$(dirname "$OUTLOG_DEST")" 2>/dev/null || true
    cp "$OUTLOG_CONFINED" "$OUTLOG_DEST" 2>/dev/null || true
fi

# Best-effort: on any non-clean run, surface the per-test verbose log if present.
cat_outlog_on_failure() {
    if [ -n "$OUTLOG_DEST" ] && [ -f "$OUTLOG_DEST" ]; then
        echo
        echo "===== ui_test per-test verbose log ($OUTLOG_DEST) ====="
        cat "$OUTLOG_DEST"
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
if [ "$FAILED" != "0" ]; then
    cat_outlog_on_failure
    exit 1
fi
exit 0
