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
# Per-test verbose-log dump. SMATCHET_UI_TEST_OUTLOG is the DURABLE destination path
# the caller (CI step) later cats + uploads as an artifact. It is deliberately NOT
# passed verbatim to `ui_test.run --outLog=`: since PR #1566 the app confines outLog
# under <userDataDir>/ui-tests/ and REJECTS absolute paths / `..` traversal
# (Source/Core/include/Commands/PathConfinement.h). So we pass only a confinement-legal
# RELATIVE name (the basename) to the command, let the app write the resolved log under
# the throwaway profile's ui-tests/ subdir, then copy it back out to OUTLOG_DEST (below)
# so the caller's cat/upload — and this script's own on-failure dump — can find it. We
# `cat` it on failure for diagnosis (the --spawn parent only prints pass/fail counts).
OUTLOG_DEST="${SMATCHET_UI_TEST_OUTLOG:-}"
OUTLOG_REL=""
if [ -n "$OUTLOG_DEST" ]; then
    OUTLOG_REL="$(basename "$OUTLOG_DEST")"
fi

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
if [ -n "$OUTLOG_REL" ]; then
    # Relative arg only — the app resolves it under <userDataDir>/ui-tests/ (confined).
    OUTLOG_ARG=(--outLog="$OUTLOG_REL")
    echo "  outLog (relative, confined under <userData>/ui-tests/): $OUTLOG_REL"
    echo "  outLog durable destination: $OUTLOG_DEST"
fi

RAW_OUTPUT="$(SMATCHET_USER_DATA="$TMPDIR_DATA" \
    SMATCHET_TEST_JIRA_BACKEND_FIXTURE="$FIXTURE" \
    "$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
    "${OUTLOG_ARG[@]}" \
    --mcp-port="$TEST_PORT" 2>&1 || true)"

echo "$RAW_OUTPUT" | tail -40

# The app wrote the confined log to <userDataDir>/ui-tests/<relative>. Here the
# user-data dir is the throwaway $TMPDIR_DATA (deleted by the EXIT trap), so copy the
# resolved log out to the durable destination NOW — before this script exits — so both
# the caller's cat/upload and cat_outlog_on_failure below can read it.
if [ -n "$OUTLOG_REL" ]; then
    RESOLVED_OUTLOG="$TMPDIR_DATA/ui-tests/$OUTLOG_REL"
    if [ -f "$RESOLVED_OUTLOG" ]; then
        mkdir -p "$(dirname "$OUTLOG_DEST")"
        cp "$RESOLVED_OUTLOG" "$OUTLOG_DEST"
    fi
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
