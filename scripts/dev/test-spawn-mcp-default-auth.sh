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
# Two phases, both under the secure default (McpRequireTokenOnLoopback ON):
#   1. NO persisted token → parent mints a per-spawn token, injects it into the
#      child env, sends it on the wire. This is the exact #1566 break condition.
#   2. A persisted operator token (smatchet_config.json mcp_auth_token) → parent
#      must send THAT token (not a freshly-minted one) and inject NOTHING into the
#      child env. This is the CR #4 configured-token path: before the
#      provision/request split the parent always minted+sent a fresh token while
#      the child required the configured one → 401. Phase 2 locks that fix.
# Both force the secure default by UNSETTING any
# SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK override (the CI opt-out PR A added was
# removed once this product fix landed — see the workflow NOTEs; the unset stays
# as a defensive guard against a developer-local override) and point
# SMATCHET_USER_DATA at a fresh dir so no ambient operator token leaks in.
#
# Exit codes:
#   0 — both phases returned ok:true under the secure default (handshake works)
#   1 — a phase failed (the #1566 regression, or a token-handshake break)
#   2 — exe missing (cannot run)

set -euo pipefail

EXE="${SMATCHET_EXE:-build/ninja-iter-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-iter-msvc --target SmatchetStandalone" >&2
    echo "Passed: 0  Failed: 0"
    exit 2
fi

# to_native — emit a Windows-native path for SMATCHET_USER_DATA when on MSYS/Cygwin.
to_native() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -m "$1"
    else
        printf '%s' "$1"
    fi
}

# run_spawn_phase <label> — assumes SMATCHET_USER_DATA + the secure-default env are
# already exported. Runs `app.version --spawn` and echoes PASS/FAIL. Returns 0 on
# ok:true, 1 otherwise. Shared by both phases so there is one assertion path
# (Pillar 5 — no copy-paste between the two phases).
run_spawn_phase() {
    local label="$1"
    echo "[test-spawn-mcp-default-auth] ($label) SMATCHET_USER_DATA=$SMATCHET_USER_DATA"
    echo "[test-spawn-mcp-default-auth] ($label) launching ephemeral Smatchet via app.version --spawn ..."

    local raw json_line ok errcode
    raw="$("$EXE" cmd app.version --spawn --yes 2>&1 || true)"
    echo "$raw" | tail -25

    json_line="$(echo "$raw" | grep -oE '\{.*\}' | tail -1 || true)"
    if [ -z "$json_line" ]; then
        echo "FAIL ($label): --spawn produced no JSON envelope (instance never became reachable — the #1566 symptom)." >&2
        return 1
    fi

    ok="$(echo "$json_line" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print('1' if d.get('ok') is True else '0')" 2>/dev/null || echo "0")"
    errcode="$(echo "$json_line" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('error',{}).get('code','') if isinstance(d.get('error'),dict) else '')" 2>/dev/null || echo "")"

    if [ "$ok" = "1" ]; then
        echo "PASS ($label): --spawn reached MCP-ready and returned ok:true under the secure default."
        return 0
    fi

    echo "FAIL ($label): --spawn did not return ok:true (error.code='$errcode')." >&2
    echo "      A 'timeout' here = the #1566 regression (tokenless probe); 'not-connected' with a 401" >&2
    echo "      message = the token handshake itself is broken." >&2
    return 1
}

PASS=0
FAIL=0

# --- Phase 1: secure default, NO persisted token (mint+inject path) ----------
# Fresh empty user-data dir → no smatchet_config.json → compiled defaults
# (McpRequireTokenOnLoopback=true, mcp_auth_token empty): the exact #1566 break
# condition. Parent mints a per-spawn token, injects via env, sends on the wire.
TMP_EMPTY="$(mktemp -d 2>/dev/null || mktemp -d -t smatchet-spawn-auth)"
# --- Phase 2: secure default, PERSISTED operator token (configured path) ------
# A separate fresh dir holding a smatchet_config.json with an operator token.
# Parent must send THAT token and inject nothing (CR #4 path).
TMP_CONFIGURED="$(mktemp -d 2>/dev/null || mktemp -d -t smatchet-spawn-auth-cfg)"
cleanup() { rm -rf "$TMP_EMPTY" "$TMP_CONFIGURED"; }
trap cleanup EXIT

# Crucial for both phases: force the SECURE default. PR A exports this =false at
# workflow level to unblock CI; this test asserts the product fix works WITHOUT
# that opt-out. Also drop any ambient spawn token so it can't mask a break.
unset SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK
unset SMATCHET_MCP_SPAWN_TOKEN

# ---- Phase 1 run ----
SMATCHET_USER_DATA="$(to_native "$TMP_EMPTY")"
export SMATCHET_USER_DATA
echo
echo "=== Phase 1: secure default, no persisted token (parent mints + injects) ==="
if run_spawn_phase "no-token"; then PASS=$((PASS + 1)); else FAIL=$((FAIL + 1)); fi

# ---- Phase 2 run ----
# Write a plaintext mcp_auth_token: ConfigManager::Load() reads it via the
# mcp_auth_token plaintext fallback, then re-seals it to mcp_auth_token_enc
# (DPAPI/Keystore) on first load — sequentially, before the child spawns, so the
# child reads the sealed token back. mcp_require_token_on_loopback is pinned true
# so the env-unset above can't be the thing that makes it pass.
CONFIGURED_TOKEN="cfgtoken_$(printf '%s' "$TMP_CONFIGURED" | tr -cd 'a-f0-9' | tail -c 12)deadbeef"
CFG_DIR_NATIVE="$(to_native "$TMP_CONFIGURED")"
cat > "$TMP_CONFIGURED/smatchet_config.json" <<JSON
{
  "mcp_auth_token": "$CONFIGURED_TOKEN",
  "mcp_require_token_on_loopback": true
}
JSON
SMATCHET_USER_DATA="$CFG_DIR_NATIVE"
export SMATCHET_USER_DATA
echo
echo "=== Phase 2: secure default, persisted operator token (parent sends configured, injects nothing) ==="
if run_spawn_phase "configured-token"; then PASS=$((PASS + 1)); else FAIL=$((FAIL + 1)); fi

echo
echo "Passed: $PASS  Failed: $FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
