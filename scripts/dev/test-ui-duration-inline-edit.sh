#!/usr/bin/env bash
# test-ui-duration-inline-edit.sh — bucket-E driver for the inline duration-cell editor's
# COMMIT / CANCEL contract (tests/ui/duration_inline_edit_commit.test.cpp). Invokes `ui_test.run`
# against an ephemeral spawn, parses the JSON envelope, reports Passed / Failed.
#
# Covers the three recently-fixed RenderTextInlineEdit bugs end-to-end through the real ImGui
# duration editor: a no-op edit never PUTs (dirty-check), focus-loss ends the edit even when
# unchanged, and Escape cancels — plus the positive commit (type → Enter). Pairs with the pure-unit
# suite tests/Core/TicketFieldEditorCommitPolicyPure.test.cpp.
#
# Spawn-warmup retry: the headless `--spawn` runner intermittently launches the ephemeral app
# "cold" — the test-engine context misses its warmup window and the ENTIRE queued test set fails
# 0-of-N (a whole-suite wipeout, distinct from a genuine partial failure). This is the documented
# `--spawn` flake (spawn_warmup_deterministic_gate.test.cpp) and hits every bucket-E test equally.
# We retry ONLY on that wipeout signal (passed==0 && failed==total), never on a partial failure.
#
# Exit codes:
#   0 — every test passed
#   1 — at least one test failed (genuine)
#   2 — binary missing OR build had SMATCHET_BUILD_UI_TESTS=OFF

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-ui-test-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
TEST_PORT="${SMATCHET_TEST_PORT:-58741}"
# imgui_test_engine's filter is substring-match with optional ^ (anchor-start) / $ (anchor-end) —
# NOT a glob. "DurationInlineEdit" is the test category, matching all four registered variants.
FILTER="${UI_TEST_FILTER:-DurationInlineEdit}"
# Number of cold-spawn warmup retries before giving up (override for slow machines).
MAX_ATTEMPTS="${UI_TEST_WARMUP_RETRIES:-8}"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone" >&2
    exit 2
fi

# Isolated user-data dir so this run never writes to the developer's real profile.
TMPDIR_DATA="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_DATA"' EXIT

# Pre-seed the config: a fresh profile shows the first-launch Whisper setup banner
# (##WhisperSetupBanner), which floats OVER the test window and swallows the engine's
# ItemClick / MouseClick / KeyChars — silently failing every interaction-driven bucket-E test.
# Marking setup completed (and backend-reachable) keeps the run isolated AND banner-free. Same
# recipe as test-ui-grid-pane-windows.sh.
cat > "$TMPDIR_DATA/smatchet_config.json" <<'CFG'
{"read_only_mode": false, "whisper_setup_completed": true, "backend_has_been_reachable": true}
CFG

field() {
    # field <json> <key> — extract data.<key>, default "?".
    echo "$1" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('$2','?'))" 2>/dev/null || echo "?"
}

PASSED="?"; FAILED="?"; TESTED="?"; LOG="?"
attempt=1
while [ "$attempt" -le "$MAX_ATTEMPTS" ]; do
    PORT=$((TEST_PORT + attempt - 1))
    echo "[test-ui-duration-inline-edit] attempt $attempt/$MAX_ATTEMPTS — launching ephemeral Smatchet (port $PORT)..."
    RAW_OUTPUT="$(SMATCHET_USER_DATA="$TMPDIR_DATA" \
        "$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
        --mcp-port="$PORT" 2>&1 || true)"
    echo "$RAW_OUTPUT" | tail -8

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

# Guard against a zero-test run silently reporting success (filter typo / dropped registration).
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
