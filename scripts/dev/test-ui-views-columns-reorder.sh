#!/usr/bin/env bash
# test-ui-views-columns-reorder.sh — bucket-E driver for the Views → Columns
# drag-reorder flake hunt. Invokes `ui_test.run` against an ephemeral spawn,
# parses the JSON envelope, and reports Passed / Failed.
#
# Exit codes:
#   0 — every test passed
#   1 — at least one test failed
#   2 — binary missing OR build had SMATCHET_BUILD_UI_TESTS=OFF

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-ui-test-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
TEST_PORT="${SMATCHET_TEST_PORT:-58732}"
# imgui_test_engine's filter is substring-match with optional ^ (anchor-start) /
# $ (anchor-end) and comma-separated terms — NOT a glob. "ColumnsReorder"
# matches both _NoWidths / _WithWidths variants registered in
# tests/ui/views_columns_reorder.test.cpp.
FILTER="${UI_TEST_FILTER:-ColumnsReorder}"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone" >&2
    exit 2
fi

extract() {
    local jq_path="$1"
    "$PY" -c "import sys,json,re; t=sys.stdin.read(); m=re.search(r'\{.*\}',t,re.S); d=json.loads(m.group(0)); v=d; \
import functools; \
[v := v.get(k) if isinstance(v,dict) else None for k in '$jq_path'.split('.')]; \
print(json.dumps(v) if v is not None else 'MISSING')" \
        2>/dev/null || echo "MISSING"
}

echo "[test-ui-views-columns-reorder] launching ephemeral Smatchet (port $TEST_PORT)..."
RAW_OUTPUT="$("$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
    --port="$TEST_PORT" 2>&1 || true)"

echo "$RAW_OUTPUT" | tail -40

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
