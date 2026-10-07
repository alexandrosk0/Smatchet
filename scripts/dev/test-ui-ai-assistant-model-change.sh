#!/usr/bin/env bash
# test-ui-ai-assistant-model-change.sh — bucket-E driver for the F2 model-change
# auto-clear + warning strip ("[model changed - chat cleared]"). Drives two
# Assistant turns through AiAssistantController with a stub IAiClient (provider
# flips Anthropic -> DeepSeek between turns), then asserts the cleared history +
# the warning-strip string. Invokes `ui_test.run` against an ephemeral spawn,
# parses the JSON envelope, reports Passed / Failed.
#
# Exit codes:
#   0 — every test passed
#   1 — at least one test failed
#   2 — binary missing OR build had SMATCHET_BUILD_UI_TESTS=OFF

set -euo pipefail

# Shared bucket-E preamble (exe staleness guard, throwaway profile, wedge-proof capture).
# shellcheck source=scripts/dev/lib/ui-test-driver.sh
. "$(dirname "$0")/lib/ui-test-driver.sh"

EXE="${SMATCHET_EXE:-build/ninja-ui-test-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
TEST_PORT="${SMATCHET_TEST_PORT:-58743}"
# Substring filter matches AssistantModelChange_ClearsHistoryAndSetsStrip.
FILTER="${UI_TEST_FILTER:-AssistantModelChange}"
# Hard wall-clock cap for the ephemeral spawn so a hung worker join / dispatcher
# never wedges CI. ui_test_capture guards on `timeout` being present (git-bash on
# Windows may lack it) and falls back to an unguarded run with a warning.
RUN_TIMEOUT_SECS="${SMATCHET_RUN_TIMEOUT_SECS:-180}"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone" >&2
    exit 2
fi

ui_test_require_fresh_exe "$EXE" || exit 2

# Throwaway seeded profile (scripts/dev/lib/ui-test-driver.sh): the test saves
# stub provider keys through ConfigManager, which must never land in the
# developer's real config, and no first-run banner or update modal sits over
# the widgets under test.
ui_test_isolate_home --seed

echo "[test-ui-ai-assistant-model-change] launching ephemeral Smatchet (port $TEST_PORT)..."
# Captured through a FILE, not `$(...)`: a --spawn grandchild that outlives the
# CLI would hold a command-substitution pipe open forever.
ui_test_capture "$RUN_TIMEOUT_SECS" "$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
    --mcp-port="$TEST_PORT"
RAW_OUTPUT="$UI_TEST_OUTPUT"

echo "$RAW_OUTPUT" | tail -40

if ui_test_timed_out; then
    echo "FAIL: ui_test.run exceeded ${RUN_TIMEOUT_SECS}s hard timeout (hung spawn?) — aborted." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

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
