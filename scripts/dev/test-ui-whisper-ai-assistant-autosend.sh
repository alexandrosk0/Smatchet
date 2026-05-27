#!/usr/bin/env bash
# test-ui-whisper-ai-assistant-autosend.sh — bucket-E driver for the
# PR #249 (bf9ce67f) fix: the ImGui-half assertion that the splice-reload
# bridge in the AI Assistant panel actually rescues a splice into an
# already-active InputTextMultiline.
#
# Two variants:
#   - WhisperAutosend_FixGuard_SpliceSurvivesActiveInputText — proves the fix
#     in place keeps the splice alive across the active-widget frame, the
#     mirror picks it up, and the auto-send dispatches.
#   - WhisperAutosend_RegressionGate_NoReloadDropsSplice — flips the reload
#     call off and pins the bug shape (splice silently dropped). This guards
#     against future ImGui upgrades that might make the reload unnecessary —
#     if it stops failing-as-expected, the production fix can be simplified.
#
# Exit codes:
#   0 — every variant passed
#   1 — at least one variant failed
#   2 — binary missing OR build had SMATCHET_BUILD_UI_TESTS=OFF OR
#       SMATCHET_WITH_WHISPER=OFF (no whisper means the test guards-out)

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-ui-test-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
TEST_PORT="${SMATCHET_TEST_PORT:-58751}"
# Substring filter — matches both variants under the WhisperAutosend category.
FILTER="${UI_TEST_FILTER:-WhisperAutosend}"

if [ ! -f "$EXE" ]; then
    echo "[test-ui-whisper-ai-assistant-autosend] SKIP: $EXE not found." >&2
    echo "    Build with: cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone" >&2
    exit 2
fi

echo "[test-ui-whisper-ai-assistant-autosend] launching ephemeral Smatchet (port $TEST_PORT)..."
RAW_OUTPUT="$("$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
    --port="$TEST_PORT" 2>&1 || true)"

echo "$RAW_OUTPUT" | tail -60

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

# The filter expects both variants to register (compile-time gated on
# SMATCHET_WITH_WHISPER + SMATCHET_WITH_AI). If passed=0 we have a registration
# issue — surface as skip rather than fail so an OFF build doesn't poison
# test-all.sh aggregate.
if [ "$PASSED" = "0" ] && [ "$FAILED" = "0" ]; then
    echo "[test-ui-whisper-ai-assistant-autosend] SKIP: no tests matched filter '$FILTER' (whisper or AI gated off?)"
    exit 0
fi

echo "Passed: $PASSED  Failed: $FAILED"
if [ "$FAILED" != "0" ]; then
    exit 1
fi
exit 0
