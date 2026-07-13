#!/usr/bin/env bash
# test-ui-mcp-live-http-auth.sh — bucket-E driver for the MCP live-HTTP
# `Authorize` integration test (AGENTIC_INFRA_AUDIT.md finding C6).
#
# Drives `ui_test.run --name=Authorize_RealSocket` against an ephemeral spawn of
# the bucket-E exe. The test starts a SECOND McpPlugin instance on its own port
# and asserts over a real httplib socket: 200 with a valid token, 401 without /
# with a wrong token, 403 on a DNS-rebind Host and on a cross-origin Origin, and
# 503 once kMaxConcurrentSseConnections SSE streams are held open.
#
# Unlike the fresh-state-race driver this needs no sanitizer — the oracle is the
# ui_test.run JSON envelope (failed=0 over a matched test), not an ASan report.
# A plain ninja-ui-test build is sufficient.
#
# Build first (any bucket-E preset with SMATCHET_WITH_MCP=ON, the default):
#   cmake --preset ninja-ui-test-msvc
#   cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone
# then:
#   SMATCHET_EXE=build/ninja-ui-test-msvc/Smatchet.exe \
#     bash scripts/dev/test-ui-mcp-live-http-auth.sh
#
# Exit codes:
#   0 — test passed (failed=0 over a matched test)
#   1 — test failed OR matched 0 tests
#   2 — binary missing OR build had SMATCHET_BUILD_UI_TESTS=OFF

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-ui-test-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
# The rig's own MCP port. The test constructs its OWN plugin on a different port
# (see mcp_live_http_auth.test.cpp), so this only forces --spawn to launch a
# fresh child rather than attaching to a stale running instance.
TEST_PORT="${SMATCHET_TEST_PORT:-58745}"
# imgui_test_engine's filter is substring-match. "Authorize_RealSocket" matches
# McpLiveHttp/Authorize_RealSocket.
FILTER="${UI_TEST_FILTER:-Authorize_RealSocket}"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with:" >&2
    echo "  cmake --preset ninja-ui-test-msvc" >&2
    echo "  cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone" >&2
    exit 2
fi

echo "[test-ui-mcp-live-http-auth] launching ephemeral Smatchet (exe=$EXE port=$TEST_PORT)..."
RAW_OUTPUT="$("$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
    --mcp-port="$TEST_PORT" 2>&1 || true)"

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
    echo "FAIL: build had SMATCHET_BUILD_UI_TESTS=OFF — rebuild with a ninja-ui-test preset." >&2
    echo "Passed: 0  Failed: 0"
    exit 2
fi

if [ "$PASSED" = "?" ] || [ "$FAILED" = "?" ]; then
    echo "Passed: 0  Failed: 1"
    exit 1
fi

# Zero-match guard (fail-closed): a renamed/dropped registration or a filter typo
# makes ui_test.run match 0 tests and report passed=0 failed=0. A driver that
# runs 0 tests is broken, not passing — the Authorize regression could re-land
# undetected. Mirrors test-ui-mcp-lua-fresh-state-race.sh.
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
