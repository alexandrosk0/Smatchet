#!/usr/bin/env bash
# test-ui-automation-reload-hooks-race.sh — bucket-E driver for Issue #1693
# (automation.reload-hooks racing the shared main lua_State when dispatched
# off the UI thread, e.g. via MCP).
#
# Drives `ui_test.run --name=ConcurrentReloadHooks` against an ephemeral spawn
# of the ASan-instrumented bucket-E exe. The test dispatches the REAL
# `automation.reload-hooks` command (CommandSource::Mcp, mirroring McpPlugin's
# direct Commands().Dispatch(...) call from its httplib worker thread) in a
# tight bounded loop concurrently with the UI thread driving the real
# DrawLuaWindows against the main `lua` state. Pre-fix this raced the Lua heap
# (RunLuaSetupScript ran inline on the dispatching thread); post-fix the
# command hops onto the UI thread via RunOnUiThreadAsCommandResult, so the two
# threads never touch the lua_State at once.
#
# Sibling of test-ui-mcp-lua-fresh-state-race.sh (same harness shape, distinct
# hazard — see BuiltinCommands_Automation.cpp's automation.reload-hooks
# comment + tests/ui/automation_reload_hooks_race.test.cpp header).
#
# Primary oracle: ZERO AddressSanitizer reports. Secondary oracle: the
# ui_test.run JSON envelope reports failed=0.
#
# Build the instrumented exe first (MSVC ASan; Clang ASan also works):
#   cmake --preset ninja-ui-test-asan-msvc
#   cmake --build --preset ninja-ui-test-asan-msvc --target SmatchetStandalone
# then:
#   SMATCHET_EXE=build/ninja-ui-test-asan-msvc/Smatchet.exe \
#     bash scripts/dev/test-ui-automation-reload-hooks-race.sh
#
# Exit codes:
#   0 — test passed AND zero sanitizer reports
#   1 — test failed OR a sanitizer report was emitted
#   2 — binary missing OR build had SMATCHET_BUILD_UI_TESTS=OFF

set -euo pipefail

# Default to the MSVC ASan UI-test build (this is the preset most reliably
# available on a Windows dev box without a Clang toolchain); override with
# SMATCHET_EXE for the Clang ASan build or a plain ninja-ui-test-msvc exe
# (non-sanitized smoke — passes but proves nothing about the heap race).
EXE="${SMATCHET_EXE:-build/ninja-ui-test-asan-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
TEST_PORT="${SMATCHET_TEST_PORT:-58745}"
# imgui_test_engine's filter is substring-match (NOT a glob). "ConcurrentReloadHooks"
# matches AutomationReloadHooksRace/ConcurrentReloadHooks_ConcurrentDrawLuaWindows.
FILTER="${UI_TEST_FILTER:-ConcurrentReloadHooks}"
# Hard wall-clock cap so a hung worker join / UI-thread hop (e.g. a regression
# reintroducing the automation.reload-hooks wedge this test guards against)
# never wedges CI — the test's own bounded frame loop cannot force a stuck
# std::thread::join() to return (no-detach is an absolute rule here), so the
# process-level timeout is the actual backstop. Same convention as
# test-ui-ai-assistant-model-change.sh; guarded on `timeout` being present
# (git-bash on Windows may lack it).
RUN_TIMEOUT_SECS="${SMATCHET_RUN_TIMEOUT_SECS:-180}"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with:" >&2
    echo "  cmake --preset ninja-ui-test-asan-msvc" >&2
    echo "  cmake --build --preset ninja-ui-test-asan-msvc --target SmatchetStandalone" >&2
    exit 2
fi

echo "[test-ui-automation-reload-hooks-race] launching ephemeral Smatchet (exe=$EXE port=$TEST_PORT)..."
export ASAN_OPTIONS="${ASAN_OPTIONS:-abort_on_error=1:halt_on_error=1:symbolize=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"

if command -v timeout >/dev/null 2>&1; then
    RAW_OUTPUT="$(timeout "$RUN_TIMEOUT_SECS" "$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
        --mcp-port="$TEST_PORT" 2>&1 || true)"
else
    echo "[test-ui-automation-reload-hooks-race] warning: 'timeout' not found — running without wall-clock guard." >&2
    RAW_OUTPUT="$("$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
        --mcp-port="$TEST_PORT" 2>&1 || true)"
fi

echo "$RAW_OUTPUT" | tail -60

if echo "$RAW_OUTPUT" | grep -qE "(==ERROR: AddressSanitizer:|runtime error:|UndefinedBehaviorSanitizer:|LeakSanitizer:)"; then
    echo >&2
    echo "FAIL: sanitizer report detected — the automation.reload-hooks race regression has re-opened." >&2
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
    echo "FAIL: build had SMATCHET_BUILD_UI_TESTS=OFF — rebuild with ninja-ui-test-asan-msvc." >&2
    echo "Passed: 0  Failed: 0"
    exit 2
fi

if [ "$PASSED" = "?" ] || [ "$FAILED" = "?" ]; then
    echo "Passed: 0  Failed: 1"
    exit 1
fi

# Zero-match guard (fail-closed): a renamed/dropped registration or a filter
# typo makes ui_test.run match 0 tests and report passed=0 failed=0. Without
# this a 0-test run exits green with ZERO coverage. Mirrors
# test-ui-mcp-lua-fresh-state-race.sh.
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
