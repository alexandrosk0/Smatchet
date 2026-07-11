#!/usr/bin/env bash
# test-ui-views-columns-reorder.sh — bucket-E driver for the Views → Columns
# drag-reorder flake hunt. Invokes `ui_test.run` against an ephemeral spawn,
# parses the JSON envelope, and reports Passed / Failed.
#
# Exit codes:
#   0 — every test passed
#   1 — at least one test failed (or the filter matched zero tests — zero-test guard, #77)
#   2 — binary missing OR build had SMATCHET_BUILD_UI_TESTS=OFF
#
# selftest: asserts-failure

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-ui-test-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
TEST_PORT="${SMATCHET_TEST_PORT:-58732}"
# imgui_test_engine's filter is substring-match with optional ^ (anchor-start) /
# $ (anchor-end) and comma-separated terms — NOT a glob. "ColumnsReorder"
# matches both _NoWidths / _WithWidths variants registered in
# tests/ui/views_columns_reorder.test.cpp.
FILTER="${UI_TEST_FILTER:-ColumnsReorder}"

# Given the ui_test.run JSON envelope, print "Passed: X  Failed: Y" and return
# 0 (all passed) / 1 (a failure, unparseable, or a ZERO-test vanished suite, #77)
# / 2 (build had UI tests OFF).
evaluate_ui_json() {
    local json="$1" passed failed logv
    passed="$(printf '%s' "$json" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('passed','?'))" 2>/dev/null || echo "?")"
    failed="$(printf '%s' "$json" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('failed','?'))" 2>/dev/null || echo "?")"
    logv="$(printf '%s' "$json" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('log','?'))" 2>/dev/null || echo "?")"
    echo "Result: passed=$passed failed=$failed log=$logv" >&2
    if [ "$logv" = "build had SMATCHET_BUILD_UI_TESTS=OFF" ]; then
        echo "FAIL: build had SMATCHET_BUILD_UI_TESTS=OFF — rebuild with ninja-ui-test-msvc." >&2
        echo "Passed: 0  Failed: 0"
        return 2
    fi
    if [ "$passed" = "?" ] || [ "$failed" = "?" ]; then
        echo "Passed: 0  Failed: 1"
        return 1
    fi
    # #77 zero-test guard: the substring FILTER matching ZERO registered tests
    # yields passed=0 failed=0. Without this a vanished/renamed suite passes green.
    if [ "$passed" -eq 0 ] && [ "$failed" -eq 0 ]; then
        echo "FAIL: the ui-test filter matched ZERO tests — vanished/renamed? (zero-test guard)" >&2
        echo "Passed: 0  Failed: 1"
        return 1
    fi
    echo "Passed: $passed  Failed: $failed"
    [ "$failed" != "0" ] && return 1
    return 0
}

# --selftest: prove the zero-test guard fires, a healthy run passes, and a real
# failure fails — with crafted ui_test.run envelopes (no build/spawn needed).
run_selftest() {
    if evaluate_ui_json '{"data":{"passed":0,"failed":0,"log":"ok"}}' >/dev/null 2>&1; then
        echo "test-ui-views-columns-reorder --selftest: FAIL — a zero-test (vanished) run passed" >&2
        return 1
    fi
    if ! evaluate_ui_json '{"data":{"passed":3,"failed":0,"log":"ok"}}' >/dev/null 2>&1; then
        echo "test-ui-views-columns-reorder --selftest: FAIL — a healthy run was rejected" >&2
        return 1
    fi
    if evaluate_ui_json '{"data":{"passed":3,"failed":1,"log":"ok"}}' >/dev/null 2>&1; then
        echo "test-ui-views-columns-reorder --selftest: FAIL — a failing run passed" >&2
        return 1
    fi
    echo "test-ui-views-columns-reorder --selftest: PASS — zero-test guard fires; healthy passes; failures fail."
    return 0
}

if [ "${1:-}" = "--selftest" ]; then
    run_selftest
    exit $?
fi

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone" >&2
    exit 2
fi

echo "[test-ui-views-columns-reorder] launching ephemeral Smatchet (port $TEST_PORT)..."
RAW_OUTPUT="$("$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
    --mcp-port="$TEST_PORT" 2>&1 || true)"

echo "$RAW_OUTPUT" | tail -40

# Find the JSON envelope (the `cmd` runner prints `{...}` to stdout last).
JSON_LINE="$(echo "$RAW_OUTPUT" | grep -oE '\{.*\}' | tail -1 || true)"
if [ -z "$JSON_LINE" ]; then
    echo "FAIL: could not extract JSON envelope from ui_test.run output" >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

echo
# Parse + guard (incl. the #77 zero-test guard) via the testable helper.
evaluate_ui_json "$JSON_LINE" && rc=0 || rc=$?
exit "$rc"
