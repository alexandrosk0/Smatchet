#!/usr/bin/env bash
# test-ui-jira-deterministic-backend.sh — bucket-E driver for the deterministic
# Jira backend UI tests (Slice 4 of deterministic-jira-test-backend.md).
# Sets an isolated SMATCHET_USER_DATA dir, injects the basic-grid fixture via
# SMATCHET_TEST_JIRA_BACKEND_FIXTURE, invokes `ui_test.run`, and parses the
# JSON envelope.
#
# NOTE on --outLog: this driver runs ui_test.run through --spawn, which resolves a
# relative --outLog to an ABSOLUTE path before forwarding it to the ephemeral child.
# Since #1566 the child confines --outLog under <userData>/ui-tests/ and rejects
# absolute paths, so a per-test outLog cannot be requested here without teaching the
# --spawn forwarder to preserve a relative path. It is therefore omitted; the
# RAW_OUTPUT dump below (spawn output + JSON envelope) carries the pass/fail signal.
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
if [ "$FAILED" != "0" ]; then
    exit 1
fi
exit 0
