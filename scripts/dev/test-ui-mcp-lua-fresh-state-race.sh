#!/usr/bin/env bash
# test-ui-mcp-lua-fresh-state-race.sh — bucket-E driver for the MCP-Lua
# fresh-per-call sol::state race regression (docs/design/mcp-lua-fresh-state-race.md).
#
# Drives `ui_test.run --name=FreshState` against an ephemeral spawn of the
# ASan-instrumented bucket-E exe. The test runs a worker thread firing the real
# ExecuteLuaSnippetForMcp / ExecuteLuaMcpTool (each building a FRESH sol::state)
# concurrently with the UI thread driving the real DrawLuaWindows against the
# main `lua` state. Pre-fix this raced the Lua heap and AddressSanitizer aborted;
# post-fix it is clean.
#
# Primary oracle: ZERO AddressSanitizer reports (the exe aborts non-zero with an
# "==ERROR: AddressSanitizer:" dump on a violation). Secondary oracle: the
# ui_test.run JSON envelope reports failed=0.
#
# Build the instrumented exe first (Clang ASan+UBSan preferred; MSVC ASan also
# works). From a 14.38-pinned VS Developer Command Prompt environment:
#   cmake --preset ninja-ui-test-asan-clang
#   cmake --build --preset ninja-ui-test-asan-clang --target SmatchetStandalone
# then:
#   SMATCHET_EXE=build/ninja-ui-test-asan-clang/Smatchet.exe \
#     bash scripts/dev/test-ui-mcp-lua-fresh-state-race.sh
#
# Exit codes:
#   0 — test passed AND zero sanitizer reports
#   1 — test failed OR a sanitizer report was emitted
#   2 — binary missing OR build had SMATCHET_BUILD_UI_TESTS=OFF

set -euo pipefail

# Default to the clang ASan UI-test build; override with SMATCHET_EXE for the
# MSVC ASan build (build/ninja-ui-test-asan-msvc/Smatchet.exe) or a plain
# ninja-ui-test-msvc exe (non-sanitized smoke — passes but proves nothing about
# the heap race).
EXE="${SMATCHET_EXE:-build/ninja-ui-test-asan-clang/Smatchet.exe}"
PY="${PYTHON:-python}"
TEST_PORT="${SMATCHET_TEST_PORT:-58744}"
# imgui_test_engine's filter is substring-match (NOT a glob). "FreshState"
# matches McpLuaRace/FreshState_ConcurrentDrawLuaWindows.
FILTER="${UI_TEST_FILTER:-FreshState}"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with:" >&2
    echo "  cmake --preset ninja-ui-test-asan-clang" >&2
    echo "  cmake --build --preset ninja-ui-test-asan-clang --target SmatchetStandalone" >&2
    exit 2
fi

echo "[test-ui-mcp-lua-fresh-state-race] launching ephemeral Smatchet (exe=$EXE port=$TEST_PORT)..."
# ASan options: abort (don't continue) on the first error so the run fails fast
# and the dump is unambiguous; symbolize for a readable stack.
export ASAN_OPTIONS="${ASAN_OPTIONS:-abort_on_error=1:halt_on_error=1:symbolize=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"

# The CLI parses --mcp-port=<int> (CliCommandRunner.cpp); --port is silently
# ignored. A non-default port forces --spawn to launch a fresh child against the
# just-built ASan exe rather than attaching to a stale running instance.
RAW_OUTPUT="$("$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
    --mcp-port="$TEST_PORT" 2>&1 || true)"

echo "$RAW_OUTPUT" | tail -60

# A sanitizer report is a hard fail regardless of the JSON envelope — the engine
# may report "passed" if the corruption manifested after the assertions but the
# process still aborts. Scan the captured combined stdout+stderr.
if echo "$RAW_OUTPUT" | grep -qE "(==ERROR: AddressSanitizer:|runtime error:|UndefinedBehaviorSanitizer:|LeakSanitizer:)"; then
    echo >&2
    echo "FAIL: sanitizer report detected — the fresh-state race regression has re-opened." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

# Find the JSON envelope (the `cmd` runner prints `{...}` to stdout last).
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
    echo "FAIL: build had SMATCHET_BUILD_UI_TESTS=OFF — rebuild with ninja-ui-test-asan-clang." >&2
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
