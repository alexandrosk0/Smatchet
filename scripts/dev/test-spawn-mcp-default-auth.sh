#!/usr/bin/env bash
# test-spawn-mcp-default-auth.sh — regression gate for the --spawn MCP auth handshake.
#
# This is the test that would have caught #1566. PR #1566 flipped the
# `McpRequireTokenOnLoopback` default false→true; the --spawn parent's probe sent
# NO token, so the secure-default child returned 401 and the parent polled to its
# 30 s timeout → every --spawn CI gate hung. PR B (spawn token handshake) makes the
# parent auto-provision a per-spawn token and send it on every request, so --spawn
# works under the secure default with NO env opt-out.
#
# The assertion: under the DEFAULT secure config (McpRequireTokenOnLoopback ON, no
# persisted mcp_auth_token), `app.version --spawn` reaches MCP-ready and returns
# ok:true. We force the secure default by UNSETTING any
# SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK override (PR A sets it false workflow-wide;
# this test must prove the product fix, not the CI opt-out) and by pointing
# SMATCHET_USER_DATA at a fresh empty dir so no operator-configured token leaks in.
#
# Exit codes:
#   0 — --spawn returned ok:true under the secure default (handshake works)
#   1 — --spawn failed (the #1566 regression, or a token-handshake break)
#   2 — exe missing (cannot run)

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-iter-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-iter-msvc --target SmatchetStandalone" >&2
    echo "Passed: 0  Failed: 0"
    exit 2
fi

# --- hermetic, secure-default environment -----------------------------------
# Fresh empty user-data dir → no config.json → compiled defaults
# (McpRequireTokenOnLoopback=true, mcp_auth_token empty): the exact #1566 break
# condition the spawn token handshake must now satisfy. Both parent and child
# (same exe) inherit this env, so they agree on the dir.
TMP_DATA="$(mktemp -d 2>/dev/null || mktemp -d -t smatchet-spawn-auth)"
cleanup() { rm -rf "$TMP_DATA"; }
trap cleanup EXIT
if command -v cygpath >/dev/null 2>&1; then
    export SMATCHET_USER_DATA="$(cygpath -m "$TMP_DATA")"
else
    export SMATCHET_USER_DATA="$TMP_DATA"
fi
# Crucial: force the SECURE default. PR A exports this =false at workflow level to
# unblock CI; this test asserts the product fix works WITHOUT that opt-out.
unset SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK
# Don't let an ambient spawn/auth token leak in and mask a real handshake break.
unset SMATCHET_MCP_SPAWN_TOKEN

echo "[test-spawn-mcp-default-auth] SMATCHET_USER_DATA=$SMATCHET_USER_DATA (fresh, secure default)"
echo "[test-spawn-mcp-default-auth] launching ephemeral Smatchet via app.version --spawn ..."
RAW_OUTPUT="$("$EXE" cmd app.version --spawn --yes 2>&1 || true)"

echo "$RAW_OUTPUT" | tail -25

JSON_LINE="$(echo "$RAW_OUTPUT" | grep -oE '\{.*\}' | tail -1 || true)"
if [ -z "$JSON_LINE" ]; then
    echo "FAIL: --spawn produced no JSON envelope (instance never became reachable — the #1566 symptom)." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

OK="$(echo "$JSON_LINE" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print('1' if d.get('ok') is True else '0')" 2>/dev/null || echo "0")"
ERRCODE="$(echo "$JSON_LINE" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('error',{}).get('code','') if isinstance(d.get('error'),dict) else '')" 2>/dev/null || echo "")"

echo
if [ "$OK" = "1" ]; then
    echo "PASS: --spawn reached MCP-ready and returned ok:true under the secure default (token handshake works)."
    echo "Passed: 1  Failed: 0"
    exit 0
fi

echo "FAIL: --spawn did not return ok:true under the secure default (error.code='$ERRCODE')." >&2
echo "      A 'timeout' here = the #1566 regression (tokenless probe); 'not-connected' with a 401" >&2
echo "      message = the token handshake itself is broken." >&2
echo "Passed: 0  Failed: 1"
exit 1
